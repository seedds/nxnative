import Foundation

// MARK: - Device Deployment & Transfer Extensions for SaveSyncService
extension SaveSyncService {
    public func pushSaveToDevice(
        titleId: String,
        bundleId: String,
        device: ConnectedAppleDevice,
        completion: @escaping (Bool, String) -> Void
    ) {
        let saveDir = resolvedSaveDirectory(forTitleId: titleId)
        guard fileManager.fileExists(atPath: saveDir.path) else {
            completion(false, "No local save found for Title ID \(titleId). Play on Mac first to create save data.")
            return
        }

        let realBundleId = DeviceDeploymentService.shared.findBundleId(for: titleId, on: device) ?? bundleId

        DispatchQueue.global(qos: .userInitiated).async {
            let process = Process()
            let pipe = Pipe()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
            process.arguments = [
                "devicectl", "device", "copy", "to",
                "--device", device.id,
                "--domain-type", "appDataContainer",
                "--domain-identifier", realBundleId,
                "--source", saveDir.path,
                "--destination", "Documents"
            ]
            process.standardOutput = pipe
            process.standardError = pipe

            do {
                try process.run()
                process.waitUntilExit()
                let success = process.terminationStatus == 0
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                let out = String(decoding: data, as: UTF8.self)
                DispatchQueue.main.async {
                    if success {
                        completion(true, "Save synchronized successfully!")
                    } else {
                        if out.contains("not found") || out.contains("Bundle") {
                            completion(false, "App not found on iPhone. Install the .ipa port first.")
                        } else {
                            completion(false, out.trimmingCharacters(in: .whitespacesAndNewlines))
                        }
                    }
                }
            } catch {
                DispatchQueue.main.async { completion(false, error.localizedDescription) }
            }
        }
    }

    public func pullSaveFromDevice(
        titleId: String,
        bundleId: String,
        device: ConnectedAppleDevice,
        completion: @escaping (Bool, String) -> Void
    ) {
        _ = backupMacSave(titleId: titleId)
        let saveDir = resolvedSaveDirectory(forTitleId: titleId)
        let realBundleId = DeviceDeploymentService.shared.findBundleId(for: titleId, on: device) ?? bundleId

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let process = Process()
            let pipe = Pipe()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
            process.arguments = [
                "devicectl", "device", "copy", "from",
                "--device", device.id,
                "--domain-type", "appDataContainer",
                "--domain-identifier", realBundleId,
                "--source", "Documents/save",
                "--destination", saveDir.deletingLastPathComponent().path
            ]
            process.standardOutput = pipe
            process.standardError = pipe

            do {
                try process.run()
                process.waitUntilExit()
                let success = process.terminationStatus == 0
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                let out = String(decoding: data, as: UTF8.self)
                DispatchQueue.main.async {
                    self?.objectWillChange.send()
                    if success {
                        completion(true, "Save imported successfully!")
                    } else {
                        if out.contains("not found") {
                            completion(false, "Save data or app does not exist on device yet.")
                        } else {
                            completion(false, out.trimmingCharacters(in: .whitespacesAndNewlines))
                        }
                    }
                }
            } catch {
                DispatchQueue.main.async { completion(false, error.localizedDescription) }
            }
        }
    }
}
