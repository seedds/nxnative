import XCTest
@testable import NXMotion
final class MotionContractTests:XCTestCase {
    @MainActor func testSharedPersistenceAndCoordinates() throws {
        let dir=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at:dir) }
        let url=dir.appendingPathComponent("dsu.json"),store=SensorStore(url:url)
        XCTAssertTrue(store.sensors.isEmpty)
        store.add();let phone=try XCTUnwrap(store.sensors.first)
        phone.host="127.0.0.1";phone.port="26760";phone.slot=1
        phone.applyCalibration(map:AxisMap(x:2,y:-1,z:3),bias:SIMD3(0.1,0.2,0.3),gravity:SIMD3(0,-1,0),preview:Orientation())
        let saved=try DSUConfiguration.load(from:url)
        XCTAssertEqual(saved.devices[0].axes,[2,-3,-1])
        XCTAssertEqual(try saved.environment()["DM_DSU_1"],"127.0.0.1,26760,1,2,-3,-1,0.1,0.2,0.3")
        let restored=SensorStore(url:url)
        XCTAssertEqual(restored.sensors.first?.id,phone.id)
        XCTAssertEqual(restored.sensors.first?.axes,phone.axes)
        XCTAssertEqual(restored.sensors.first?.gyroBias,phone.gyroBias)
    }
    func testInvalidDisabledRecordRejected() throws {
        var d=DSUDevice(id:UUID(),name:"Bad",player:1,host:"127.0.0.1",port:26760,slot:1,axes:[1,2,3],bias:[0,0,0],enabled:false)
        d.displayAxes=[Int.min,2,3]
        XCTAssertThrowsError(try DSUConfiguration(devices:[d]).environment())
        d.displayAxes=nil;d.axes=[]
        XCTAssertThrowsError(try DSUConfiguration(devices:[d]).environment())
    }
}
