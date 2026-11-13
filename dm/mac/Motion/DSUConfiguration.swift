// Shared, per-user motion configuration. Never embed LAN endpoints in game bundles.
import Foundation

enum DSURole: String, Codable, CaseIterable {
    case full, left, right
    var title: String { switch self { case .full: return "Full controller"; case .left: return "Joy-Con Left"; case .right: return "Joy-Con Right" } }
}

struct DSUDevice: Codable, Equatable, Identifiable {
    var id: UUID
    var name: String
    var player: Int
    var host: String
    var port: Int
    var slot: Int
    // Axial gyro transform: signed DSU axes -> Switch coordinates.
    var axes: [Int]
    var bias: [Double]
    var enabled: Bool
    var role: DSURole? = nil // Missing in v1 documents means full; never inferred from slot/model.
    var railSL: Int? = nil
    var railSR: Int? = nil
    var buttons: Bool? = nil
    var calibrated: Bool? = nil
    var displayAxes: [Int]? = nil
    var valid: Bool {
        let octets=host.split(separator:".",omittingEmptySubsequences:false)
        return octets.count==4 && octets.allSatisfy { part in
            !part.isEmpty && part.allSatisfy { $0.isASCII && $0.isNumber } && UInt8(part) != nil
        } && (1...8).contains(player) && (1...65535).contains(port) && (0...3).contains(slot)
            && [railSL ?? -1,railSR ?? -1].allSatisfy { (-1...15).contains($0) }
            && axes.count==3 && axes.allSatisfy { (-3...3).contains($0) && $0 != 0 }
            && Set(axes.map { abs($0) })==Set([1,2,3])
            && (displayAxes == nil || (displayAxes!.count==3 && displayAxes!.allSatisfy { (-3...3).contains($0) && $0 != 0 } && Set(displayAxes!.map { abs($0) })==Set([1,2,3])))
            && bias.count==3 && bias.allSatisfy { $0.isFinite && abs($0)<=10000 }
    }
    var environmentValue: String? {
        guard valid else { return nil }
        return ([host,String(port),String(slot)] + axes.map(String.init) + bias.map { String($0) }).joined(separator:",")
    }
}
struct DSUConfiguration: Codable, Equatable {
    var version = 1
    var devices: [DSUDevice] = []
    static var userURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/nxnative/motion/dsu-v1.json")
    }
    // Duplicate enabled player assignments fail closed, rather than silently choosing a phone.
    func environment() throws -> [String:String] {
        guard version==1,devices.count<=32 else { throw Failure.invalid }
        var result: [String:String]=[:]
        guard Set(devices.map(\.id)).count==devices.count else { throw Failure.invalid }
        for device in devices {
            guard device.valid else { throw Failure.invalid }
        }
        for device in devices where device.enabled {
            guard let value=device.environmentValue else { throw Failure.invalid }
            let key="DM_DSU_"+String(device.player)
            guard result[key]==nil else { throw Failure.duplicatePlayer }
            result[key]=value
            result["DM_DSU_RAIL_"+String(device.player)]=String(device.railSL ?? -1)+","+String(device.railSR ?? -1)
            result["DM_DSU_ROLE_"+String(device.player)]=(device.role ?? .full).rawValue
            result["DM_DSU_BUTTONS_"+String(device.player)]=(device.buttons ?? false) ? "1":"0"
        }
        return result
    }
    static func load(from url: URL = userURL) throws -> DSUConfiguration {
        guard FileManager.default.fileExists(atPath:url.path) else { return DSUConfiguration() }
        let size=(try url.resourceValues(forKeys:[.fileSizeKey])).fileSize ?? 0
        guard size<=65536 else { throw Failure.invalid }
        let config=try JSONDecoder().decode(Self.self,from:Data(contentsOf:url))
        _ = try config.environment();return config
    }
    func save(to url:URL = userURL) throws {
        _ = try environment()
        try FileManager.default.createDirectory(at:url.deletingLastPathComponent(),withIntermediateDirectories:true)
        let data=try JSONEncoder().encode(self)
        try data.write(to:url,options:.atomic)
        try FileManager.default.setAttributes([.posixPermissions:0o600],ofItemAtPath:url.path)
    }
    enum Failure: Error { case invalid,duplicatePlayer }
}
