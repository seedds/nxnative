import Foundation

public struct SaveComparisonInfo: Equatable {
    public let macSaveURL: URL
    public let macModificationDate: Date?
    public let macFileCount: Int
    public let hasDeviceSave: Bool
}

public final class SaveSyncService: ObservableObject {
    public static let shared = SaveSyncService()
    private let fileManager = FileManager.default

    private init() {}

    public func macSaveDirectory(for titleId: String) -> URL {
        let appSupport = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let base = appSupport.appendingPathComponent("nxnative/\(titleId.uppercased())", isDirectory: true)
        let saveDir = base.appendingPathComponent("save", isDirectory: true)
        let workDir = base.appendingPathComponent("save.work", isDirectory: true)

        if fileManager.fileExists(atPath: saveDir.path) {
            let files = (try? fileManager.contentsOfDirectory(atPath: saveDir.path)) ?? []
            if !files.isEmpty { return saveDir }
        }
        if fileManager.fileExists(atPath: workDir.path) {
            return workDir
        }
        return saveDir
    }

    public func inspectMacSave(for titleId: String) -> SaveComparisonInfo {
        let dir = macSaveDirectory(for: titleId)
        guard fileManager.fileExists(atPath: dir.path) else {
            return SaveComparisonInfo(macSaveURL: dir, macModificationDate: nil, macFileCount: 0, hasDeviceSave: false)
        }

        let contents = (try? fileManager.contentsOfDirectory(atPath: dir.path)) ?? []
        let newest = contents.compactMap { name -> Date? in
            let path = dir.appendingPathComponent(name).path
            return (try? fileManager.attributesOfItem(atPath: path)[.modificationDate]) as? Date
        }.max()

        return SaveComparisonInfo(
            macSaveURL: dir,
            macModificationDate: newest,
            macFileCount: contents.count,
            hasDeviceSave: false
        )
    }

    public func backupMacSave(titleId: String) -> URL? {
        let saveDir = macSaveDirectory(for: titleId)
        guard fileManager.fileExists(atPath: saveDir.path) else { return nil }

        let backupRoot = saveDir.deletingLastPathComponent().appendingPathComponent("backups", isDirectory: true)
        try? fileManager.createDirectory(at: backupRoot, withIntermediateDirectories: true)

        let timestamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let destination = backupRoot.appendingPathComponent("save_backup_\(timestamp)")

        do {
            try fileManager.copyItem(at: saveDir, to: destination)
            return destination
        } catch {
            return nil
        }
    }

    public func pushSaveToDevice(titleId: String, bundleId: String, device: ConnectedAppleDevice, completion: @escaping (Bool, String) -> Void) {
        let saveDir = macSaveDirectory(for: titleId)
        guard fileManager.fileExists(atPath: saveDir.path) else {
            completion(false, "Nenhum save local encontrado para Title ID \(titleId). Jogue primeiro no Mac para criar o save.")
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
                        completion(true, "Save sincronizado com sucesso!")
                    } else {
                        if out.contains("not found") || out.contains("Bundle") {
                            completion(false, "App não encontrado no iPhone. Instale o port .ipa no dispositivo primeiro.")
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

    public func pullSaveFromDevice(titleId: String, bundleId: String, device: ConnectedAppleDevice, completion: @escaping (Bool, String) -> Void) {
        _ = backupMacSave(titleId: titleId)
        let saveDir = macSaveDirectory(for: titleId)
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
                        completion(true, "Save importado com sucesso!")
                    } else {
                        if out.contains("not found") {
                            completion(false, "Save ou App ainda não existe no dispositivo.")
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
