import Foundation
import AppKit

public struct ConnectedAppleDevice: Identifiable, Equatable {
    public let id: String
    public let name: String
    public let model: String
    public let osVersion: String
    public let isPaired: Bool
}

public struct DeveloperSigningTeam: Identifiable, Equatable {
    public let id: String
    public let name: String
}

public enum DeploymentErrorCategory: Equatable {
    case signatureVerificationFailed
    case deviceLocked
    case developerModeDisabled
    case insufficientStorage
    case deviceDisconnected
    case invalidIPA
    case generic(String)
}

public struct DeploymentProgressInfo: Equatable {
    public enum Phase: String {
        case idle = "Idle"
        case extracting = "Extracting Package (.ipa)"
        case signing = "Preparing Signature"
        case connecting = "Connecting to Device"
        case transferring = "Transferring to Device"
        case installing = "Installing on iOS"
        case completed = "Installation Completed"
        case failed = "Installation Failed"
        case cancelled = "Deployment Cancelled"
    }

    public var phase: Phase = .idle
    public var progressFraction: Double = 0.0 // 0.0 ... 1.0
    public var statusMessage: String = ""
    public var detailMessage: String = ""
    public var logLines: [String] = []
    public var isRunning: Bool = false
    public var targetDeviceName: String = ""
    public var elapsedSeconds: Int = 0
    public var errorCategory: DeploymentErrorCategory? = nil
    public var rawError: String? = nil
}

public final class DeviceDeploymentService: ObservableObject {
    public static let shared = DeviceDeploymentService()

    @Published public var connectedDevices: [ConnectedAppleDevice] = []
    @Published public var signingTeams: [DeveloperSigningTeam] = []
    @Published public var isDetecting: Bool = false
    @Published public var deploymentStatus: String? = nil
    @Published public var activeDeployment: DeploymentProgressInfo = DeploymentProgressInfo()

    internal var currentRunningProcess: Process? = nil
    internal var completionCallback: ((Bool, String) -> Void)? = nil
    internal var elapsedTimer: Timer? = nil

    private init() {
        refreshDevicesAndTeams()
    }

    public func refreshDevicesAndTeams() {
        guard !isDetecting else { return }
        isDetecting = true

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let devices = self.fetchConnectedDevices()
            let teams = self.fetchSigningIdentities()

            DispatchQueue.main.async {
                self.connectedDevices = devices
                self.signingTeams = teams
                self.isDetecting = false
            }
        }
    }

    public func dismissDeploymentStatus() {
        if !activeDeployment.isRunning {
            activeDeployment = DeploymentProgressInfo()
        }
    }

    public func cancelDeployment() {
        guard activeDeployment.isRunning else { return }
        stopElapsedTimer()
        currentRunningProcess?.terminate()
        currentRunningProcess = nil

        activeDeployment.isRunning = false
        activeDeployment.phase = .cancelled
        activeDeployment.statusMessage = "Deployment cancelled by user."
        activeDeployment.detailMessage = "The installation process was safely terminated."

        let cb = completionCallback
        completionCallback = nil
        cb?(false, "Cancelled by user.")
    }

    public func shareViaAirDrop(fileURL: URL) {
        DispatchQueue.main.async {
            if let service = NSSharingService(named: .sendViaAirDrop) {
                if service.canPerform(withItems: [fileURL]) {
                    service.perform(withItems: [fileURL])
                }
            } else {
                NSWorkspace.shared.activateFileViewerSelecting([fileURL])
            }
        }
    }

    public func installApp(
        appBundleURL: URL,
        on device: ConnectedAppleDevice,
        completion: @escaping (Bool, String) -> Void
    ) {
        // Prevent concurrent deployments
        if activeDeployment.isRunning {
            completion(false, "Another deployment is already in progress.")
            return
        }

        self.completionCallback = completion

        // Initialize active deployment state
        activeDeployment = DeploymentProgressInfo(
            phase: .extracting,
            progressFraction: 0.05,
            statusMessage: "Starting installation on \(device.name)...",
            detailMessage: "Preparing game package for transfer...",
            logLines: ["==> Starting deployment of \(appBundleURL.lastPathComponent) on \(device.name)..."],
            isRunning: true,
            targetDeviceName: device.name,
            elapsedSeconds: 0,
            errorCategory: nil,
            rawError: nil
        )

        startElapsedTimer()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            var targetAppURL = appBundleURL

            // 1. If given an .ipa archive, extract Payload/*.app with cache support
            if appBundleURL.pathExtension.lowercased() == "ipa" {
                let (appURL, extractError) = self.prepareExtractedApp(from: appBundleURL)
                guard let extractedURL = appURL else {
                    DispatchQueue.main.async {
                        self.failDeployment(
                            category: .invalidIPA,
                            title: "Invalid .ipa Archive",
                            detail: extractError ?? "Could not extract Payload/*.app from .ipa file."
                        )
                    }
                    return
                }
                targetAppURL = extractedURL
            }

            guard self.activeDeployment.isRunning else { return }

            // 2. Prepare code signature
            DispatchQueue.main.async {
                self.activeDeployment.phase = .signing
                self.activeDeployment.progressFraction = 0.35
                self.activeDeployment.statusMessage = "Verifying app signature..."
                self.activeDeployment.detailMessage = "Ensuring bundle integrity before transfer..."
            }

            self.applyCodeSignatureIfNeeded(targetAppURL: targetAppURL)

            guard self.activeDeployment.isRunning else { return }

            // 3. Execute xcrun devicectl device install app asynchronously with live line streaming
            DispatchQueue.main.async {
                self.activeDeployment.phase = .transferring
                self.activeDeployment.progressFraction = 0.45
                self.activeDeployment.statusMessage = "Transferring to \(device.name)..."
                self.activeDeployment.detailMessage = "Connecting to device and transferring game files..."
            }

            self.executeDeviceCtlInstall(targetAppURL: targetAppURL, device: device)
        }
    }

    // MARK: - App Extraction with Fast Cache
    private func prepareExtractedApp(from ipaURL: URL) -> (URL?, String?) {
        let fm = FileManager.default
        let values = try? ipaURL.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
        let size = values?.fileSize ?? 0
        let mtime = Int(values?.contentModificationDate?.timeIntervalSince1970 ?? 0)

        let safeName = ipaURL.deletingPathExtension().lastPathComponent.replacingOccurrences(of: " ", with: "_")
        let cacheDir = fm.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("nxnative/deploy_cache/\(safeName)_\(size)_\(mtime)")

        let payloadDir = cacheDir.appendingPathComponent("Payload")

        // Check if cache already contains a valid extracted .app
        if fm.fileExists(atPath: payloadDir.path),
           let items = try? fm.contentsOfDirectory(atPath: payloadDir.path),
           let existingApp = items.first(where: { $0.hasSuffix(".app") }) {
            let appURL = payloadDir.appendingPathComponent(existingApp)
            let plistURL = appURL.appendingPathComponent("Info.plist")
            if fm.fileExists(atPath: plistURL.path) {
                DispatchQueue.main.async {
                    self.activeDeployment.logLines.append("[OK] Extracted app bundle found in local cache.")
                    self.activeDeployment.progressFraction = 0.30
                }
                return (appURL, nil)
            }
        }

        // Needs extraction
        try? fm.createDirectory(at: cacheDir, withIntermediateDirectories: true)

        DispatchQueue.main.async {
            self.activeDeployment.logLines.append("==> Extracting \(ipaURL.lastPathComponent) (\(self.formatByteSize(Int64(size))))...")
            self.activeDeployment.statusMessage = "Extracting .ipa package (\(self.formatByteSize(Int64(size))))..."
            self.activeDeployment.detailMessage = "Please wait while game assets are unpacked..."
            self.activeDeployment.progressFraction = 0.15
        }

        // Try using /usr/bin/ditto -x -k first (fast multithreaded unpacker on macOS)
        let ditto = Process()
        ditto.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        ditto.arguments = ["-x", "-k", ipaURL.path, cacheDir.path]
        try? ditto.run()
        ditto.waitUntilExit()

        // Fallback to /usr/bin/unzip if ditto failed
        if ditto.terminationStatus != 0 {
            let unzip = Process()
            unzip.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
            unzip.arguments = ["-q", "-o", ipaURL.path, "-d", cacheDir.path]
            try? unzip.run()
            unzip.waitUntilExit()
        }

        guard let items = try? fm.contentsOfDirectory(atPath: payloadDir.path),
              let appFolder = items.first(where: { $0.hasSuffix(".app") }) else {
            return (nil, "The .ipa archive does not contain a valid Payload/*.app directory.")
        }

        let appURL = payloadDir.appendingPathComponent(appFolder)
        DispatchQueue.main.async {
            self.activeDeployment.logLines.append("[OK] Package extracted successfully: \(appFolder)")
            self.activeDeployment.progressFraction = 0.30
        }
        return (appURL, nil)
    }

    // MARK: - Code Signing
    private func applyCodeSignatureIfNeeded(targetAppURL: URL) {
        let teams = fetchSigningIdentities()
        let sign = Process()
        sign.executableURL = URL(fileURLWithPath: "/usr/bin/codesign")

        if let team = teams.first {
            sign.arguments = ["--force", "--deep", "--sign", team.id, targetAppURL.path]
            DispatchQueue.main.async {
                self.activeDeployment.logLines.append("==> Signing with development identity: \(team.name)")
            }
        } else {
            // Apply ad-hoc signature
            sign.arguments = ["--force", "--deep", "--sign", "-", targetAppURL.path]
            DispatchQueue.main.async {
                self.activeDeployment.logLines.append("==> Applying ad-hoc signature (--sign -)...")
            }
        }

        try? sign.run()
        sign.waitUntilExit()
    }
}
