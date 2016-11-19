//
//  ViewController.swift
//  bicycleMapApp
//
//  Created by 田中潤 on 2016/11/19.
//  Copyright © 2016年 田中潤. All rights reserved.
//

import UIKit
import MapKit
import CoreLocation
import RealmSwift

class Location: Object {
    dynamic var latitude:Double = 0
    dynamic var longitude:Double = 0
    dynamic var createdAt = NSDate(timeIntervalSince1970: 1)
}

class ViewController: UIViewController, CLLocationManagerDelegate {
    @IBOutlet weak var mapView: MKMapView!
    
    
    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view, typically from a nib.
    }
    
    override func didReceiveMemoryWarning() {
        super.didReceiveMemoryWarning()
        // Dispose of any resources that can be recreated.
    }
    
    
}

private func addCurrentLocation(rowLocation: CLLocation) {
    let location = makeLocation(rowLocation)
    let queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0)
    dispatch_async(queue) {
        // Get the default Realm
        let realm = Realm()
        realm.beginWrite()
        // Create a Location object
        realm.add(location)
        realm.commitWrite()
    }
}
private func loadSavedLocations() -> Results<Location> {
    // Get the default Realm
    let realm = Realm()
    token = realm.addNotificationBlock { [weak self] notification, realm in
        self?.tableView.reloadData()
    }
    // Load recent location objects
    return realm.objects(Location).sorted("createdAt", ascending: false)
}









