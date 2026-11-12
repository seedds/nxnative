import Foundation
@main struct ConfigurationTests {
    static func main() throws {
        let d=DSUDevice(id:UUID(),name:"Test",player:2,host:"127.0.0.1",port:26760,slot:1,axes:[1,3,-2],bias:[0.1,0,0],enabled:true)
        var config=DSUConfiguration(devices:[d]);let env=try config.environment();assert(env["DM_DSU_2"] != nil)
        var duplicate=d;duplicate.id=UUID();config.devices.append(duplicate)
        do { _ = try config.environment();fatalError("duplicate accepted") } catch DSUConfiguration.Failure.duplicatePlayer {}
        var invalid=d;invalid.axes=[Int.min,1,2];assert(!invalid.valid)
        invalid=d;invalid.host="127.0.0.1,evil";assert(!invalid.valid)
        let url=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString).appendingPathComponent("dsu.json")
        defer { try? FileManager.default.removeItem(at:url.deletingLastPathComponent()) }
        config.devices=[d];try config.save(to:url);let loaded=try DSUConfiguration.load(from:url);assert(loaded==config)
        print("DSU configuration validation, duplicate rejection and atomic round-trip passed")
    }
}
