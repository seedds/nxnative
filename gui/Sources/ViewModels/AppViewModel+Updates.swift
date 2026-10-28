import Foundation
import AppKit
import SwiftUI

extension AppViewModel {
    // MARK: - GitHub Updates & Auto-Installer
    public func checkForUpdates() {
        guard isNxnativeValid, !nxnativePath.isEmpty else { return }
        isCheckingUpdate = true
        updateChecker.checkForUpdates(nxnativePath: nxnativePath) { [weak self] info in
            self?.isCheckingUpdate = false
            self?.updateInfo = info
        }
    }
    
    /// Triggered from the macOS Menu Bar ("Check for Updates...") or Settings tab:
    /// Opens the Update modal, checks GitHub (rennan101/nxnative_Launcher), and if a new version is found, downloads & installs it automatically.
    public func openUpdateCheckerAndAutoInstall() {
        showUpdateSheet = true
        updateErrorBanner = nil
        updateSuccessBanner = nil
        updateCompletedReadyToRelaunch = false
        
        guard isNxnativeValid, !nxnativePath.isEmpty else {
            updateErrorBanner = "Local repository path not configured."
            return
        }
        
        isCheckingUpdate = true
        updateChecker.checkForUpdates(nxnativePath: nxnativePath) { [weak self] info in
            guard let self = self else { return }
            self.isCheckingUpdate = false
            self.updateInfo = info
            
            if let info = info, info.hasUpdate {
                self.applyUpdate(autoRelaunch: true)
            }
        }
    }
    
    public func applyUpdate(autoRelaunch: Bool = false) {
        guard isNxnativeValid, !nxnativePath.isEmpty else { return }
        isApplyingUpdate = true
        updateErrorBanner = nil
        updateSuccessBanner = nil
        updateCompletedReadyToRelaunch = false
        updateInstallLogs.removeAll()
        
        updateChecker.applyUpdate(
            nxnativePath: nxnativePath,
            rebuildLauncher: true,
            progressHandler: { [weak self] line in
                self?.updateInstallLogs.append(line)
            },
            completion: { [weak self] success, message in
                guard let self = self else { return }
                self.isApplyingUpdate = false
                if success {
                    self.updateSuccessBanner = message
                    self.updateErrorBanner = nil
                    self.updateCompletedReadyToRelaunch = true
                    self.checkForUpdates()
                    self.refreshEnginePatches()
                    self.refreshPrerequisites()
                    self.logs.append(LogEntry(level: .success, message: message))
                    
                    if autoRelaunch {
                        self.updateInstallLogs.append("==> Automatic update finished! Relaunching NxNative Launcher in 2 seconds...")
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
                            self?.relaunchUpdatedLauncher()
                        }
                    }
                } else {
                    self.updateErrorBanner = message
                    self.logs.append(LogEntry(level: .error, message: message))
                }
            }
        )
    }
    
    public func relaunchUpdatedLauncher() {
        updateChecker.relaunchLauncher(nxnativePath: nxnativePath)
    }
    
    // MARK: - Delegate Implementations
    public func installerDidProduceOutput(_ line: String) {
        installerLogs.append(line)
    }
    
    public func installerDidStartItem(_ name: String) {
        installerCurrentItem = name
    }
    
    public func installerDidFinishItem(_ name: String, success: Bool) {}
    
    public func installerDidCompleteAll(allSuccess: Bool) {
        installerCurrentItem = nil
    }
    
    public func updateCheckerDidFindUpdate(_ info: UpdateInfo) {
        self.logs.append(LogEntry(level: .info, message: "nxnative update available: \(info.commitMessages.count) new commits."))
    }
    
    public func updateCheckerDidProduceLog(_ message: String) {
        self.logs.append(LogEntry(level: .info, message: message))
    }
}
