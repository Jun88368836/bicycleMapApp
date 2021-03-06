////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "sync/sync_user.hpp"

#include "sync/impl/sync_metadata.hpp"
#include "sync/sync_manager.hpp"
#include "sync/sync_session.hpp"

namespace realm {

SyncUser::SyncUser(std::string refresh_token,
                   std::string identity,
                   util::Optional<std::string> server_url,
                   bool is_admin)
: m_state(State::Active)
, m_server_url(server_url.value_or(""))
, m_is_admin(is_admin)
, m_refresh_token(std::move(refresh_token))
, m_identity(std::move(identity))
{
    if (!is_admin) {
        SyncManager::shared().perform_metadata_update([this, server_url=std::move(server_url)](const auto& manager) {
            auto metadata = SyncUserMetadata(manager, m_identity);
            metadata.set_state(server_url, m_refresh_token);
        });
    }
}

std::vector<std::shared_ptr<SyncSession>> SyncUser::all_sessions()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::shared_ptr<SyncSession>> sessions;
    if (m_state == State::Error) {
        return sessions;
    }
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (auto ptr_to_session = it->second.lock()) {
            if (!ptr_to_session->is_in_error_state()) {
                sessions.emplace_back(std::move(ptr_to_session));
                it++;
                continue;
            }
        }
        // This session is bad, destroy it.
        it = m_sessions.erase(it);
    }
    return sessions;
}

std::shared_ptr<SyncSession> SyncUser::session_for_url(const std::string& url)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state == State::Error) {
        return nullptr;
    }
    auto it = m_sessions.find(url);
    if (it == m_sessions.end()) {
        return nullptr;
    }
    auto locked = it->second.lock();
    if (!locked) {
        // Remove the session from the map, because it has fatally errored out or the entry is invalid.
        m_sessions.erase(it);
    }
    return locked;
}

void SyncUser::update_refresh_token(std::string token)
{
    std::vector<std::shared_ptr<SyncSession>> sessions_to_revive;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        switch (m_state) {
            case State::Error:
                return;
            case State::Active:
                m_refresh_token = token;
                break;
            case State::LoggedOut: {
                sessions_to_revive.reserve(m_waiting_sessions.size());
                m_refresh_token = token;
                m_state = State::Active;
                for (auto& pair : m_waiting_sessions) {
                    if (auto ptr = pair.second.lock()) {
                        m_sessions[pair.first] = ptr;
                        sessions_to_revive.emplace_back(std::move(ptr));
                    }
                }
                m_waiting_sessions.clear();
                break;
            }
        }
        // Update persistent user metadata.
        if (!m_is_admin) {
            SyncManager::shared().perform_metadata_update([=](const auto& manager) {
                auto metadata = SyncUserMetadata(manager, m_identity);
                metadata.set_state(m_server_url, token);
            });
        }
    }
    // (Re)activate all pending sessions.
    // Note that we do this after releasing the lock, since the session may
    // need to access protected User state in the process of binding itself.
    for (auto& session : sessions_to_revive) {
        SyncSession::revive_if_needed(session);
    }
}

void SyncUser::log_out()
{
    if (m_is_admin) {
        // Admin users cannot be logged out.
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state == State::LoggedOut) {
        return;
    }
    m_state = State::LoggedOut;
    // Move all active sessions into the waiting sessions pool. If the user is
    // logged back in, they will automatically be reactivated.
    for (auto& pair : m_sessions) {
        if (auto ptr = pair.second.lock()) {
            ptr->log_out();
            m_waiting_sessions[pair.first] = ptr;
        }
    }
    m_sessions.clear();
    // Mark the user as 'dead' in the persisted metadata Realm.
    if (!m_is_admin) {
        SyncManager::shared().perform_metadata_update([=](const auto& manager) {
            auto metadata = SyncUserMetadata(manager, m_identity, false);
            metadata.mark_for_removal();
        });
    }
}

void SyncUser::invalidate()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = State::Error;
}

std::string SyncUser::refresh_token() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_refresh_token;
}

SyncUser::State SyncUser::state() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

void SyncUser::register_session(std::shared_ptr<SyncSession> session)
{
    const std::string& url = session->config().realm_url;
    std::unique_lock<std::mutex> lock(m_mutex);
    auto has_session = [&] (const auto& sessions) {
        auto it = sessions.find(url);
        return it != sessions.end() && !it->second.expired();
    };
    if (has_session(m_sessions) || has_session(m_waiting_sessions)) {
        throw std::invalid_argument("Can only register sessions that haven't previously been registered.");
    }
    switch (m_state) {
        case State::Active:
            // Immediately ask the session to come online.
            m_sessions[url] = session;
            if (m_is_admin) {
                session->bind_with_admin_token(m_refresh_token, session->config().realm_url);
            } else {
                lock.unlock();
                SyncSession::revive_if_needed(std::move(session));
            }
            break;
        case State::LoggedOut:
            m_waiting_sessions[url] = session;
            break;
        case State::Error:
            break;
    }
}

}
