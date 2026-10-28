import Foundation
import AppKit

// MARK: - Asynchronous devicectl Execution with Real-Time Pipe Streaming
extension DeviceDeploymentService {
    internal func executeDeviceCtlInstall(
        targetAppURL: URL,
        device: ConnectedAppleDevice,
        stagedRomfsURL: URL?
    ) {
        let process = Process()
        let pipeOut = Pipe()
        let pipeErr = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        process.arguments = ["devicectl", "device", "install", "app", "--device", device.id, targetAppURL.path, "--verbose"]
        process.standardOutput = pipeOut
        process.standardError = pipeErr

        self.currentRunningProcess = process

        var fullOutput = ""
        let lock = NSLock()

        let handleOutputData: (Data) -> Void = { [weak self] data in
            guard let self = self, !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            lock.lock()
            fullOutput += text
            lock.unlock()

            let lines = text.components(separatedBy: .newlines)
            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
                guard !trimmed.isEmpty else { continue }

                DispatchQueue.main.async {
                    self.activeDeployment.logLines.append(trimmed)
                    if self.activeDeployment.logLines.count > 60 {
                        self.activeDeployment.logLines.removeFirst()
                    }
                    self.parseDeviceCtlLine(trimmed)
                }
            }
        }

        pipeOut.fileHandleForReading.readabilityHandler = { handle in
            handleOutputData(handle.availableData)
        }
        pipeErr.fileHandleForReading.readabilityHandler = { handle in
            handleOutputData(handle.availableData)
        }

        process.terminationHandler = { [weak self] proc in
            pipeOut.fileHandleForReading.readabilityHandler = nil
            pipeErr.fileHandleForReading.readabilityHandler = nil

            let remOut = pipeOut.fileHandleForReading.readDataToEndOfFile()
            let remErr = pipeErr.fileHandleForReading.readDataToEndOfFile()
            handleOutputData(remOut)
            handleOutputData(remErr)

            lock.lock()
            let output = fullOutput
            lock.unlock()

            DispatchQueue.main.async {
                self?.handleDeviceCtlTermination(
                    status: proc.terminationStatus,
                    output: output,
                    device: device,
                    targetAppURL: targetAppURL,
                    stagedRomfsURL: stagedRomfsURL
                )
            }
        }

        do {
            try process.run()
        } catch {
            pipeOut.fileHandleForReading.readabilityHandler = nil
            pipeErr.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async {
                self.failDeployment(
                    category: .generic(error.localizedDescription),
                    title: "Failed to Launch devicectl",
                    detail: error.localizedDescription
                )
            }
        }
    }

    internal func parseDeviceCtlLine(_ line: String) {
        let lower = line.lowercased()

        if lower.contains("tunnel connection") {
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.52)
            activeDeployment.detailMessage = "USB / Network tunnel established with device..."
        } else if lower.contains("developer disk image") {
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.62)
            activeDeployment.detailMessage = "Developer services active on iOS..."
        } else if lower.contains("usage assertion") {
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.72)
            activeDeployment.detailMessage = "Transferring game package to device..."
        } else if lower.contains("staging") || lower.contains("uploading") {
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.80)
            activeDeployment.detailMessage = "Transferring files to device storage..."
        } else if lower.contains("install") || lower.contains("extracting") {
            activeDeployment.phase = .installing
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.88)
            activeDeployment.statusMessage = "Finalizing installation on iOS..."
            activeDeployment.detailMessage = "iOS is registering the app in the system..."
        }
    }

    internal func handleDeviceCtlTermination(
        status: Int32,
        output: String,
        device: ConnectedAppleDevice,
        targetAppURL: URL,
        stagedRomfsURL: URL?
    ) {
        stopElapsedTimer()
        currentRunningProcess = nil

        if status == 0 {
            activeDeployment.logLines.append("[SUCCESS] App bundle installed on \(device.name).")

            // Stage 2: Sideload romfs.img into app's Documents directory if present
            if let romfsURL = stagedRomfsURL, FileManager.default.fileExists(atPath: romfsURL.path) {
                let bundleId = extractBundleIdentifier(from: targetAppURL) ?? findBundleId(for: targetAppURL.deletingPathExtension().lastPathComponent, on: device) ?? "org.nxnative"
                executeRomfsCopy(romfsURL: romfsURL, bundleId: bundleId, device: device)
                return
            }

            activeDeployment.isRunning = false
            activeDeployment.phase = .completed
            activeDeployment.progressFraction = 1.0
            activeDeployment.statusMessage = "Port installed successfully on \(device.name)!"
            activeDeployment.detailMessage = L10n.t("settings_modal.developer_mode_tip")
            activeDeployment.logLines.append("[SUCCESS] Installation completed successfully!")

            let cb = completionCallback
            completionCallback = nil
            cb?(true, activeDeployment.statusMessage)
            return
        }

        activeDeployment.isRunning = false

        // Parse Failure Category
        if output.contains("0xe8008014") || output.contains("ApplicationVerificationFailed") || output.contains("invalid signature") || output.contains("integridade não pôde ser verificada") {
            failDeployment(
                category: .signatureVerificationFailed,
                title: "iOS Signature Verification Failed",
                detail: "iOS rejected the app because it requires a valid Apple Developer certificate or must be installed via AirDrop, AltStore, or Sideloadly.",
                raw: output
            )
        } else if output.contains("passcodeRequired") || output.contains("locked") {
            failDeployment(
                category: .deviceLocked,
                title: "Device is Locked",
                detail: "Unlock your \(device.name) with your passcode and try again.",
                raw: output
            )
        } else if output.contains("Developer Mode") || output.contains("developerModeStatus") {
            failDeployment(
                category: .developerModeDisabled,
                title: "Developer Mode Required",
                detail: "Enable Developer Mode in Settings > Privacy & Security > Developer Mode on your device, then restart.",
                raw: output
            )
        } else if output.contains("no space") || output.contains("storage") || output.contains("disc full") {
            failDeployment(
                category: .insufficientStorage,
                title: "Insufficient Storage",
                detail: "Not enough free storage space on \(device.name) to install this game.",
                raw: output
            )
        } else if output.contains("tunnel") || output.contains("disconnected") || output.contains("offline") {
            failDeployment(
                category: .deviceDisconnected,
                title: "Connection Lost",
                detail: "Check the USB cable or ensure both devices are connected to the same Wi-Fi network.",
                raw: output
            )
        } else {
            let summary = output.trimmingCharacters(in: .whitespacesAndNewlines)
            failDeployment(
                category: .generic(summary),
                title: "Installation Failed on \(device.name)",
                detail: summary.isEmpty ? "Unknown devicectl error." : summary,
                raw: output
            )
        }
    }

    internal func executeRomfsCopy(romfsURL: URL, bundleId: String, device: ConnectedAppleDevice) {
        startElapsedTimer()
        activeDeployment.isRunning = true
        activeDeployment.phase = .copyingRomFS
        activeDeployment.progressFraction = 0.90
        activeDeployment.statusMessage = L10n.t("settings_modal.copying_romfs_status")
        activeDeployment.detailMessage = L10n.t("settings_modal.copying_romfs_detail")
        activeDeployment.logLines.append("==> Transferring romfs.img (\(formatByteSize(fileSize(of: romfsURL)))) to Documents on \(device.name) for \(bundleId)...")

        let process = Process()
        let pipeOut = Pipe()
        let pipeErr = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        process.arguments = [
            "devicectl", "device", "copy", "to",
            "--device", device.id,
            "--domain-type", "appDataContainer",
            "--domain-identifier", bundleId,
            "--source", romfsURL.path,
            "--destination", "Documents/romfs.img",
            "--verbose"
        ]
        process.standardOutput = pipeOut
        process.standardError = pipeErr
        self.currentRunningProcess = process

        var fullOutput = ""
        let lock = NSLock()

        let handleOutputData: (Data) -> Void = { [weak self] data in
            guard let self = self, !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            lock.lock()
            fullOutput += text
            lock.unlock()

            let lines = text.components(separatedBy: .newlines)
            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
                guard !trimmed.isEmpty else { continue }
                DispatchQueue.main.async {
                    self.activeDeployment.logLines.append(trimmed)
                    if self.activeDeployment.logLines.count > 60 {
                        self.activeDeployment.logLines.removeFirst()
                    }
                }
            }
        }

        pipeOut.fileHandleForReading.readabilityHandler = { handle in handleOutputData(handle.availableData) }
        pipeErr.fileHandleForReading.readabilityHandler = { handle in handleOutputData(handle.availableData) }

        process.terminationHandler = { [weak self] proc in
            pipeOut.fileHandleForReading.readabilityHandler = nil
            pipeErr.fileHandleForReading.readabilityHandler = nil
            let remOut = pipeOut.fileHandleForReading.readDataToEndOfFile()
            let remErr = pipeErr.fileHandleForReading.readDataToEndOfFile()
            handleOutputData(remOut)
            handleOutputData(remErr)

            lock.lock()
            let output = fullOutput
            lock.unlock()

            DispatchQueue.main.async {
                guard let self = self else { return }
                self.stopElapsedTimer()
                self.currentRunningProcess = nil
                self.activeDeployment.isRunning = false

                if proc.terminationStatus == 0 {
                    self.activeDeployment.phase = .completed
                    self.activeDeployment.progressFraction = 1.0
                    self.activeDeployment.statusMessage = "Port installed successfully on \(device.name)!"
                    self.activeDeployment.detailMessage = L10n.t("settings_modal.developer_mode_tip")
                    self.activeDeployment.logLines.append("[SUCCESS] Game data (romfs.img) copied successfully to Documents on \(device.name)!")

                    let cb = self.completionCallback
                    self.completionCallback = nil
                    cb?(true, self.activeDeployment.statusMessage)
                } else {
                    self.failDeployment(
                        category: .generic(output),
                        title: "Failed to Copy Game Data (romfs.img)",
                        detail: "Could not transfer romfs.img to \(device.name): \(output)",
                        raw: output
                    )
                }
            }
        }

        do {
            try process.run()
        } catch {
            pipeOut.fileHandleForReading.readabilityHandler = nil
            pipeErr.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async {
                self.failDeployment(
                    category: .generic(error.localizedDescription),
                    title: "Failed to Launch devicectl copy",
                    detail: error.localizedDescription
                )
            }
        }
    }

    internal func extractBundleIdentifier(from appURL: URL) -> String? {
        let plistURL = appURL.appendingPathComponent("Info.plist")
        guard let data = try? Data(contentsOf: plistURL),
              let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any],
              let bid = plist["CFBundleIdentifier"] as? String else {
            return nil
        }
        return bid
    }

    internal func fileSize(of url: URL) -> Int64 {
        let values = try? url.resourceValues(forKeys: [.fileSizeKey])
        return Int64(values?.fileSize ?? 0)
    }

    internal func failDeployment(category: DeploymentErrorCategory, title: String, detail: String, raw: String? = nil) {
        stopElapsedTimer()
        currentRunningProcess = nil
        activeDeployment.isRunning = false
        activeDeployment.phase = .failed
        activeDeployment.errorCategory = category
        activeDeployment.statusMessage = title
        activeDeployment.detailMessage = detail
        activeDeployment.rawError = raw
        activeDeployment.logLines.append("[ERROR] \(title): \(detail)")

        let cb = completionCallback
        completionCallback = nil
        cb?(false, title)
    }

    // MARK: - Timer Management
    internal func startElapsedTimer() {
        stopElapsedTimer()
        elapsedTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            DispatchQueue.main.async {
                guard let self = self, self.activeDeployment.isRunning else { return }
                self.activeDeployment.elapsedSeconds += 1
            }
        }
    }

    internal func stopElapsedTimer() {
        elapsedTimer?.invalidate()
        elapsedTimer = nil
    }

    // MARK: - Utilities
    internal func formatByteSize(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.allowedUnits = [.useGB, .useMB]
        formatter.countStyle = .file
        return formatter.string(fromByteCount: bytes)
    }
}
