import Foundation

public enum AutoSyncStatus: Equatable {
    case deviceUpdated
    case macUpdated
    case alreadyInSync
    case noSaves
    case failed(String)
}

// MARK: - Device Deployment & Transfer Extensions for SaveSyncService
extension SaveSyncService {
    
    // MARK: - Smart Automatic Save Synchronization
    public func autoSyncSave(
        titleId: String,
        bundleId: String,
        device: ConnectedAppleDevice,
        game: PortedGame? = nil,
        completion: @escaping (AutoSyncStatus, String) -> Void
    ) {
        let saveDir = resolvedSaveDirectory(forTitleId: titleId)
        let realBundleId = DeviceDeploymentService.shared.findBundleId(for: titleId, on: device) ?? bundleId

        // 1. Safety snapshot of local / iCloud save before any sync action
        if let g = game {
            _ = createSafetySnapshot(for: g, label: "pre_autosync")
        } else {
            _ = backupMacSave(titleId: titleId)
        }

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }

            let tempDir = self.fileManager.temporaryDirectory.appendingPathComponent("nxnative_sync_\(UUID().uuidString)", isDirectory: true)
            defer {
                try? self.fileManager.removeItem(at: tempDir)
            }

            try? self.fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)

            // Step 2: Extract device save into temporary staging folder
            let pullProcess = Process()
            let pullPipe = Pipe()
            pullProcess.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
            pullProcess.arguments = [
                "devicectl", "device", "copy", "from",
                "--device", device.id,
                "--domain-type", "appDataContainer",
                "--domain-identifier", realBundleId,
                "--source", "Documents/save",
                "--destination", tempDir.path
            ]
            pullProcess.standardOutput = pullPipe
            pullProcess.standardError = pullPipe

            var deviceHasSave = false
            do {
                try pullProcess.run()
                pullProcess.waitUntilExit()
                deviceHasSave = (pullProcess.terminationStatus == 0)
            } catch {
                deviceHasSave = false
            }

            let stagedSave = tempDir.appendingPathComponent("save", isDirectory: true)
            var stagedFiles: [String] = []
            if deviceHasSave && self.fileManager.fileExists(atPath: stagedSave.path) {
                stagedFiles = (try? self.fileManager.subpathsOfDirectory(atPath: stagedSave.path)) ?? []
            }
            let validStagedFiles = stagedFiles.filter { !$0.hasPrefix(".") && !$0.isEmpty }

            // Step 3: Inspect Mac / iCloud save files
            var macFiles: [String] = []
            if self.fileManager.fileExists(atPath: saveDir.path) {
                macFiles = (try? self.fileManager.subpathsOfDirectory(atPath: saveDir.path)) ?? []
            }
            let validMacFiles = macFiles.filter { !$0.hasPrefix(".") && !$0.isEmpty }

            // Find newest file modification dates
            let deviceLatestDate: Date? = validStagedFiles.compactMap { relPath in
                let full = stagedSave.appendingPathComponent(relPath).path
                return (try? self.fileManager.attributesOfItem(atPath: full)[.modificationDate]) as? Date
            }.max()

            let macLatestDate: Date? = validMacFiles.compactMap { relPath in
                let full = saveDir.appendingPathComponent(relPath).path
                return (try? self.fileManager.attributesOfItem(atPath: full)[.modificationDate]) as? Date
            }.max()

            // Decision Matrix:
            // Case A: Neither Mac nor Device has save files
            if validStagedFiles.isEmpty && validMacFiles.isEmpty {
                DispatchQueue.main.async {
                    completion(.noSaves, L10n.t("settings_modal.sync_no_saves"))
                }
                return
            }

            // Case B: Device has no save, but Mac has save -> Automatic Push
            if validStagedFiles.isEmpty && !validMacFiles.isEmpty {
                self.pushSaveToDevice(titleId: titleId, bundleId: realBundleId, device: device) { success, msg in
                    if success {
                        completion(.deviceUpdated, L10n.t("settings_modal.sync_device_updated"))
                    } else {
                        completion(.failed(msg), msg)
                    }
                }
                return
            }

            // Case C: Mac has no save, but Device has save -> Automatic Import to Mac & iCloud
            if !validStagedFiles.isEmpty && validMacFiles.isEmpty {
                self.importStagedSaveToMac(stagedSave: stagedSave, saveDir: saveDir) { success, msg in
                    if success {
                        completion(.macUpdated, L10n.t("settings_modal.sync_mac_updated"))
                    } else {
                        completion(.failed(msg), msg)
                    }
                }
                return
            }

            // Case D: Both sides have saves -> Compare modification dates
            guard let devDate = deviceLatestDate, let macDate = macLatestDate else {
                // Fallback: If dates cannot be read, push Mac save
                self.pushSaveToDevice(titleId: titleId, bundleId: realBundleId, device: device) { success, msg in
                    if success {
                        completion(.deviceUpdated, L10n.t("settings_modal.sync_device_updated"))
                    } else {
                        completion(.failed(msg), msg)
                    }
                }
                return
            }

            let diff = devDate.timeIntervalSince(macDate)
            if abs(diff) < 2.0 {
                // Within 2 seconds: considered in-sync
                DispatchQueue.main.async {
                    completion(.alreadyInSync, L10n.t("settings_modal.sync_already_up_to_date"))
                }
            } else if diff > 0 {
                // Device save is newer -> Pull into Mac & iCloud
                self.importStagedSaveToMac(stagedSave: stagedSave, saveDir: saveDir) { success, msg in
                    if success {
                        completion(.macUpdated, L10n.t("settings_modal.sync_mac_updated"))
                    } else {
                        completion(.failed(msg), msg)
                    }
                }
            } else {
                // Mac save is newer -> Push to device
                self.pushSaveToDevice(titleId: titleId, bundleId: realBundleId, device: device) { success, msg in
                    if success {
                        completion(.deviceUpdated, L10n.t("settings_modal.sync_device_updated"))
                    } else {
                        completion(.failed(msg), msg)
                    }
                }
            }
        }
    }

    private func importStagedSaveToMac(
        stagedSave: URL,
        saveDir: URL,
        completion: @escaping (Bool, String) -> Void
    ) {
        do {
            try fileManager.createDirectory(at: saveDir, withIntermediateDirectories: true)
            let items = try fileManager.contentsOfDirectory(atPath: stagedSave.path)
            for item in items {
                let src = stagedSave.appendingPathComponent(item)
                let dst = saveDir.appendingPathComponent(item)
                if fileManager.fileExists(atPath: dst.path) {
                    try fileManager.removeItem(at: dst)
                }
                try fileManager.copyItem(at: src, to: dst)
            }
            DispatchQueue.main.async { [weak self] in
                self?.objectWillChange.send()
                completion(true, "Save imported successfully!")
            }
        } catch {
            DispatchQueue.main.async {
                completion(false, error.localizedDescription)
            }
        }
    }

    // MARK: - Auto-Sync On Launch Trigger
    public func autoSyncOnLaunch(
        for game: PortedGame,
        completion: @escaping (AutoSyncStatus?, String) -> Void
    ) {
        guard let dev = DeviceDeploymentService.shared.connectedDevices.first,
              let titleId = game.titleId, !titleId.isEmpty else {
            completion(nil, "")
            return
        }
        let bundleId = game.bundleId ?? "com.nxnative.\(titleId)"
        autoSyncSave(titleId: titleId, bundleId: bundleId, device: dev, game: game) { status, msg in
            completion(status, msg)
        }
    }

    // MARK: - Direct Push & Pull Operations
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
