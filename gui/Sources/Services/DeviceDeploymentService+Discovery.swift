import Foundation
import AppKit

// MARK: - Device and Code Signing Discovery
extension DeviceDeploymentService {
    public func findBundleId(for titleId: String, on device: ConnectedAppleDevice) -> String? {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        process.arguments = ["devicectl", "device", "info", "apps", "--device", device.id]
        process.standardOutput = pipe
        process.standardError = Pipe()
        guard (try? process.run()) != nil else { return nil }
        process.waitUntilExit()

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let out = String(decoding: data, as: UTF8.self)
        let tidLower = titleId.lowercased()

        for line in out.components(separatedBy: .newlines) {
            let lower = line.lowercased()
            if lower.contains(tidLower) {
                let tokens = line.split(whereSeparator: { $0.isWhitespace })
                if tokens.count >= 2 {
                    return String(tokens[1])
                }
            }
        }
        return nil
    }

    public func revealInFinder(fileURL: URL) {
        NSWorkspace.shared.activateFileViewerSelecting([fileURL])
    }

    internal func fetchConnectedDevices() -> [ConnectedAppleDevice] {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        process.arguments = ["devicectl", "list", "devices", "--json-output", "-"]
        process.standardOutput = pipe
        process.standardError = Pipe()

        guard (try? process.run()) != nil else { return [] }
        process.waitUntilExit()
        guard process.terminationStatus == 0 else { return [] }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let raw = String(decoding: data, as: UTF8.self)

        guard let jsonStartIndex = raw.firstIndex(of: "{") else {
            return parseDeviceTable(raw)
        }

        let jsonString = String(raw[jsonStartIndex...])
        guard let jsonData = jsonString.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: jsonData) as? [String: Any],
              let res = json["result"] as? [String: Any],
              let list = res["devices"] as? [[String: Any]] else {
            return parseDeviceTable(raw)
        }

        var results: [ConnectedAppleDevice] = []
        for d in list {
            let devProps = (d["deviceProperties"] as? [String: Any]) ?? ((d["properties"] as? [String: Any])?["state"] as? [String: Any]) ?? [:]
            let hwProps = (d["hardwareProperties"] as? [String: Any]) ?? ((d["properties"] as? [String: Any])?["hardware"] as? [String: Any]) ?? [:]

            let name = (devProps["name"] as? String) ?? "iPhone"
            let udid = (hwProps["udid"] as? String) ?? (d["identifier"] as? String) ?? ""
            let model = (hwProps["marketingName"] as? String) ?? "Apple Device"

            var os = "iOS"
            if let osNum = devProps["osVersionNumber"] as? String {
                os = osNum
            } else if let osDict = devProps["osVersionNumber"] as? [String: Any], let s = osDict["stringValue"] as? String {
                os = s
            }

            guard !udid.isEmpty else { continue }

            results.append(
                ConnectedAppleDevice(
                    id: udid,
                    name: name,
                    model: model,
                    osVersion: os,
                    isPaired: true
                )
            )
        }

        return results.isEmpty ? parseDeviceTable(raw) : results
    }

    internal func parseDeviceTable(_ text: String) -> [ConnectedAppleDevice] {
        var devices: [ConnectedAppleDevice] = []
        let lines = text.components(separatedBy: .newlines)
        for line in lines {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("Name") || trimmed.hasPrefix("---") { continue }
            let parts = line.split(whereSeparator: { $0.isWhitespace })
            guard parts.count >= 3 else { continue }
            let name = String(parts[0])
            var id = ""
            for p in parts {
                if p.count >= 16 && (p.contains("-") || p.allSatisfy({ $0.isHexDigit })) {
                    id = String(p).replacingOccurrences(of: "(UDID)", with: "")
                    break
                }
            }
            if !id.isEmpty {
                devices.append(ConnectedAppleDevice(id: id, name: name, model: "Apple Device", osVersion: "iOS", isPaired: true))
            }
        }
        return devices
    }

    internal func fetchSigningIdentities() -> [DeveloperSigningTeam] {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/security")
        process.arguments = ["find-identity", "-p", "codesigning", "-v"]
        process.standardOutput = pipe

        guard (try? process.run()) != nil else { return [] }
        process.waitUntilExit()

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let out = String(decoding: data, as: UTF8.self)

        var teams: [DeveloperSigningTeam] = []
        for line in out.components(separatedBy: .newlines) {
            if line.contains("Apple Development:") || line.contains("iPhone Developer:") {
                let parts = line.components(separatedBy: "\"")
                if parts.count >= 2 {
                    let teamName = parts[1]
                    teams.append(DeveloperSigningTeam(id: teamName, name: teamName))
                }
            }
        }
        return teams
    }
}
