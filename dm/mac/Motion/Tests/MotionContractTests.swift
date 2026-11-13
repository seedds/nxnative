import XCTest
@testable import NXMotion
final class MotionContractTests:XCTestCase {
    func testHorizontalHalfPreview() {
        XCTAssertEqual(DSU.Inputs(buttons:1 << 11).horizontalPreview(role:.left).buttons,1 << 11)
        XCTAssertEqual(DSU.Inputs(buttons:1 << 10).horizontalPreview(role:.right).buttons,1 << 10)
        let rails=DSU.Inputs(buttons:(1 << 7)|(1 << 9)).horizontalPreview(role:.left,railSL:7,railSR:9)
        XCTAssertNotEqual(rails.buttons & (1 << 6),0)
        XCTAssertNotEqual(rails.buttons & (1 << 7),0)
        let reassignedMenu=DSU.Inputs(buttons:(1 << 10)|(1 << 11)).horizontalPreview(role:.left,railSL:10,railSR:11)
        XCTAssertEqual(reassignedMenu.buttons,(1 << 6)|(1 << 7))
        for role in [DSURole.left,.right] {
            let shoulders = role == .left ? [(6,8),(8,9)] : [(7,8),(9,9)]
            for (source,target) in shoulders {
                XCTAssertEqual(DSU.Inputs(buttons:1 << source).horizontalPreview(role:role).buttons,1 << target)
            }
            for sl in 0..<16 { for sr in 0..<16 {
                let preview=DSU.Inputs(buttons:(1 << sl)|(1 << sr)).horizontalPreview(role:role,railSL:sl,railSR:sr)
                XCTAssertEqual(preview.buttons & ((1 << 6)|(1 << 7)),(1 << 6)|(1 << 7))
            } }
            let menu=DSU.Inputs(buttons:(1 << 10)|(1 << 11)).horizontalPreview(role:role)
            XCTAssertEqual(menu.buttons & ((1 << 10)|(1 << 11)),(1 << 10)|(1 << 11))
        }
        for (source,target) in [(15,0),(12,1),(14,2),(13,3)] {
            XCTAssertEqual(DSU.Inputs(buttons:1 << source).horizontalPreview(role:.left).buttons,1 << target)
        }
        let input=DSU.Inputs(leftStick:SIMD2(50,20),rightStick:SIMD2(30,10))
        XCTAssertEqual(input.horizontalPreview(role:.left).leftStick,SIMD2(-20,50))
        XCTAssertEqual(input.horizontalPreview(role:.right).leftStick,SIMD2(10,-30))
        XCTAssertEqual(input.horizontalPreview(role:.full),input)
    }
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
        XCTAssertEqual(try saved.environment()["DM_DSU_BUTTONS_1"],"0")
        phone.railSL=7;phone.railSR=9
        let railSaved=try DSUConfiguration.load(from:url)
        XCTAssertEqual(try railSaved.environment()["DM_DSU_RAIL_1"],"7,9")
        let railRestored=SensorStore(url:url)
        XCTAssertEqual(railRestored.sensors.first?.railSL,7)
        XCTAssertEqual(railRestored.sensors.first?.railSR,9)
        phone.buttons=true
        XCTAssertEqual(try DSUConfiguration.load(from:url).environment()["DM_DSU_BUTTONS_1"],"1")
    }
    @MainActor func testDetectedSlotIsUsedForGameEnvironment() throws {
        let dir=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at:dir) }
        let store=SensorStore(url:dir.appendingPathComponent("dsu.json"));store.add()
        let sensor=try XCTUnwrap(store.sensors.first)
        sensor.host="127.0.0.1";sensor.slot = -1;sensor.detectedSlot=2;sensor.calibrated=true;sensor.onEdit?()
        let saved=try DSUConfiguration.load(from:dir.appendingPathComponent("dsu.json"))
        XCTAssertEqual(saved.devices.first?.slot,2)
        XCTAssertNotNil(try saved.environment()["DM_DSU_1"])
    }
    private func inputPacket(buttons:UInt16 = 0, sticks:[UInt8] = [128,128,128,128], model:UInt8 = 2) -> Data {
        var b=[UInt8](repeating:0,count:100)
        func put(_ value:UInt64,_ offset:Int,_ count:Int) {
            for i in 0..<count { b[offset+i]=UInt8(truncatingIfNeeded:value >> (8*i)) }
        }
        b.replaceSubrange(0..<4,with:Array("DSUS".utf8));put(1001,4,2);put(84,6,2);put(0x100002,16,4)
        b[21]=2;b[22]=model;b[31]=1;put(UInt64(buttons),36,2)
        b.replaceSubrange(40..<44,with:sticks)
        put(UInt64(DSU.crc(b)),8,4)
        return Data(b)
    }
    func testAll16DSUButtonMappings() throws {
        // DSU docs list high-to-low bits: Left Down Right Up Options R3 L3 Share /
        // Square Cross Circle Triangle R1 L1 R2 L2. Expectations below run low-to-high.
        let expected:[UInt32] = [0x800,0x10,0x20,0x400,0x2000,0x4000,0x8000,0x1000,
                                 0x100,0x200,0x40,0x80,0x4,0x1,0x2,0x8]
        let names=["−","L-stick","R-stick","+","Up","Right","Down","Left","ZL","ZR","L","R","X","A","B","Y"]
        for bit in 0..<16 {
            let sample=try XCTUnwrap(DSU.decode(inputPacket(buttons:UInt16(1) << bit)))
            XCTAssertEqual(sample.inputs.buttons,expected[bit],"DSU bit \(bit)")
            XCTAssertEqual(sample.inputs.pressedNames,names[bit])
            XCTAssertEqual(sample.inputs.leftStick,.zero)
            XCTAssertEqual(sample.inputs.rightStick,.zero)
        }
        XCTAssertEqual(try XCTUnwrap(DSU.decode(inputPacket(buttons:0xffff))).inputs.buttons,0xffff)
        XCTAssertEqual(try XCTUnwrap(DSU.decode(inputPacket())).inputs.buttons,0)
    }
    func testStickEndpointsAndModelNeutrality() throws {
        for model:UInt8 in [0,1,2,3] {
            let sample=try XCTUnwrap(DSU.decode(inputPacket(sticks:[0,255,129,127],model:model)))
            XCTAssertEqual(sample.inputs.leftStick,SIMD2(-128,127))
            XCTAssertEqual(sample.inputs.rightStick,SIMD2(1,-1))
        }
        let legacy=DSU.Sample(slot:0,server:1,sequence:1,timestamp:1,acceleration:.zero,gyro:.zero)
        XCTAssertEqual(legacy.inputs,DSU.Inputs())
    }
    @MainActor func testLiveInputsAcceptanceAndClearing() throws {
        let sensor=Sensor()
        var sample=try XCTUnwrap(DSU.decode(inputPacket(buttons:1,sticks:[0,255,255,0])))
        sample.sequence=1;sensor.accept(sample)
        XCTAssertEqual(sensor.inputs,sample.inputs)
        var ignored=sample;ignored.inputs=DSU.Inputs()
        sensor.accept(ignored) // Duplicate sequence must not alter displayed inputs.
        XCTAssertEqual(sensor.inputs,sample.inputs)
        ignored.sequence=2;ignored.slot=1;sensor.accept(ignored) // Other auto-selected slot.
        XCTAssertEqual(sensor.inputs,sample.inputs)
        sensor.expireInputs(now:Date().addingTimeInterval(4))
        XCTAssertEqual(sensor.inputs,DSU.Inputs())
        sample.sequence=2;sensor.accept(sample)
        XCTAssertEqual(sensor.inputs,sample.inputs)
        sample.sequence=3;sample.inputs=DSU.Inputs();sensor.accept(sample)
        XCTAssertEqual(sensor.inputs,DSU.Inputs())
        sample.sequence=4;sample.inputs.buttons=1;sensor.accept(sample)
        sensor.disconnect()
        XCTAssertEqual(sensor.inputs,DSU.Inputs())
    }
    @MainActor func testRolesAndLegacyDefault() throws {
        let dir=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at:dir) }
        let url=dir.appendingPathComponent("dsu.json"),store=SensorStore(url:url)
        store.add();let phone=try XCTUnwrap(store.sensors.first)
        phone.host="127.0.0.1";phone.calibrated=true
        for role in DSURole.allCases {
            phone.role=role
            XCTAssertEqual(try DSUConfiguration.load(from:url).devices.first?.role,role)
            XCTAssertEqual(SensorStore(url:url).sensors.first?.role,role)
            XCTAssertEqual(try store.configuration().environment()["DM_DSU_ROLE_1"],role.rawValue)
        }
        var legacy=try store.configuration();legacy.devices[0].role=nil
        try legacy.save(to:url)
        XCTAssertEqual(SensorStore(url:url).sensors.first?.role,.full)
        XCTAssertEqual(try legacy.environment()["DM_DSU_ROLE_1"],"full")
    }
    func testInvalidDisabledRecordRejected() throws {
        var d=DSUDevice(id:UUID(),name:"Bad",player:1,host:"127.0.0.1",port:26760,slot:1,axes:[1,2,3],bias:[0,0,0],enabled:false)
        d.displayAxes=[Int.min,2,3]
        XCTAssertThrowsError(try DSUConfiguration(devices:[d]).environment())
        d.displayAxes=nil;d.axes=[]
        XCTAssertThrowsError(try DSUConfiguration(devices:[d]).environment())
    }
}
