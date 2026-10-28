import Foundation
import AppKit

extension AppViewModel {
    // MARK: - Dynamic Dock & Minimized Taskbar Icon
    public func updateDockIcon() {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            NSApplication.shared.setActivationPolicy(.regular)
            
            let isDark = NSApp?.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            let resName: String
            switch self.iconAppearanceMode {
            case .light: resName = "AppIcon-Light"
            case .dark: resName = "AppIcon-Dark"
            case .mono: resName = "AppIcon-Mono"
            case .auto: resName = isDark ? "AppIcon-Dark" : "AppIcon-Light"
            }
            
            let url = Bundle.main.url(forResource: resName, withExtension: "png")
                ?? Bundle.main.url(forResource: resName, withExtension: "icns")
                ?? Bundle.main.url(forResource: "AppIcon-Light", withExtension: "png")
                ?? Bundle.main.url(forResource: "AppIcon", withExtension: "icns")
            
            if let iconURL = url, let img = NSImage(contentsOf: iconURL) {
                NSApplication.shared.applicationIconImage = img
                NSApplication.shared.dockTile.display()
                
                for window in NSApplication.shared.windows {
                    window.miniwindowImage = img
                    window.miniwindowTitle = "NxNative Launcher"
                }
            }
        }
    }
    
    public func setupWindowMinimizeIconObserver() {
        NotificationCenter.default.addObserver(
            forName: NSWindow.willMiniaturizeNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            self?.updateDockIcon()
            if let window = notification.object as? NSWindow,
               let icon = NSApplication.shared.applicationIconImage {
                window.miniwindowImage = icon
                window.miniwindowTitle = "NxNative Launcher"
            }
        }
        
        NotificationCenter.default.addObserver(
            forName: NSWindow.didBecomeKeyNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.updateDockIcon()
        }
    }
    
    // MARK: - Compatibility
    public func loadCompatibleGames(forceRefresh: Bool = false) {
        isLoadingCompatibility = true
        CompatibilityService.shared.fetchCompatibleGames(localNxnativePath: nxnativePath) { [weak self] games, isOnline in
            guard let self = self else { return }
            self.compatibleGames = games
            self.isCompatibilityFromOnline = isOnline
            self.compatibilityLastUpdated = Date()
            self.isLoadingCompatibility = false
        }
    }
    
    public func cycleCompatibilityFilter(forward: Bool) {
        let ramFilters: [Int?] = [nil, 4, 6, 8, 16]
        SoundEffectService.shared.play(.focus)
        
        let currentIdx = ramFilters.firstIndex(where: { $0 == compatibilityRamFilter }) ?? 0
        if forward {
            let nextIdx = (currentIdx + 1) % ramFilters.count
            compatibilityRamFilter = ramFilters[nextIdx]
        } else {
            let prevIdx = (currentIdx - 1 + ramFilters.count) % ramFilters.count
            compatibilityRamFilter = ramFilters[prevIdx]
        }
    }
    
    // MARK: - Environment Detection
    public func detectEnvironment(customRyujinxDir: String? = nil) {
        if let savedKeys = UserDefaults.standard.string(forKey: StorageKeys.savedProdKeysPath),
           FileManager.default.fileExists(atPath: savedKeys),
           savedKeys.hasSuffix("prod.keys") {
            self.prodKeysPath = savedKeys
            self.prodKeysSource = "Saved Preference"
            self.isProdKeysValid = true
        } else if let keysResult = SystemDetector.shared.detectProdKeys(additionalSearchDir: customRyujinxDir) {
            self.prodKeysPath = keysResult.path
            self.prodKeysSource = keysResult.sourceName
            self.isProdKeysValid = keysResult.isValid
        } else {
            self.isProdKeysValid = false
            self.prodKeysSource = "Not Found"
        }
        
        let nxResult = SystemDetector.shared.detectNxnativeDirectory()
        self.nxnativePath = nxResult.path
        self.isNxnativeValid = nxResult.isValid
        
        refreshEnginePatches()
        refreshPrerequisites()
    }
    
    public func refreshEnginePatches() {
        guard !nxnativePath.isEmpty else { return }
        self.enginePatches = PatchService.shared.checkPatchStatuses(nxnativePath: nxnativePath)
    }
    
    public func applyEnginePatches() {
        guard !nxnativePath.isEmpty else { return }
        let (applied, skipped, errors) = PatchService.shared.applyPatchesIfNeeded(nxnativePath: nxnativePath) { [weak self] msg, ok in
            self?.logs.append(LogEntry(level: ok ? .success : .info, message: msg))
        }
        refreshEnginePatches()
        if !errors.isEmpty {
            self.logs.append(LogEntry(level: .warning, message: "Engine patches completed with notices: \(errors.joined(separator: ", "))"))
        } else {
            self.logs.append(LogEntry(level: .success, message: "Engine patches verified: \(applied) applied, \(skipped) already present."))
        }
    }
    
    public func installAppleCommandLineTools() {
        showCltInstallAlert = true
        installer.install(dependency: prerequisites.xcode) { [weak self] _ in
            self?.refreshPrerequisites()
        }
    }
    
    public func refreshPrerequisites() {
        isCheckingPrerequisites = true
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let res = SystemDetector.shared.checkPrerequisites()
            DispatchQueue.main.async {
                self?.prerequisites = res
                self?.isCheckingPrerequisites = false
            }
        }
    }
    
    public func cloneNxnativeRepository() {
        let dest = URL(fileURLWithPath: defaultInstallPath).appendingPathComponent("nxnative")
        isCloningRepository = true
        cloneErrorMessage = nil
        
        SystemDetector.shared.cloneNxnativeRepository(to: dest) { [weak self] success, output in
            guard let self = self else { return }
            self.isCloningRepository = false
            if success {
                PatchService.shared.applyPatchesIfNeeded(nxnativePath: dest.path)
                self.detectEnvironment()
                self.logs.append(LogEntry(level: .success, message: "nxnative repository cloned and compatibility patches applied."))
            } else {
                self.cloneErrorMessage = output
                self.logs.append(LogEntry(level: .error, message: "Failed to clone repository: \(output)"))
            }
        }
    }
    
    // MARK: - Tool Installer / Repair / Uninstall
    public func installAllMissingDependencies() {
        let missing = prerequisites.missingList
        guard !missing.isEmpty else { return }
        showInstallerSheet = true
        isInstallingDependency = true
        installerLogs.removeAll()
        installerLogs.append("Installing \(missing.count) missing dependencies...")
        
        installer.installAll(dependencies: missing) { [weak self] success in
            self?.isInstallingDependency = false
            self?.refreshPrerequisites()
            if success {
                self?.logs.append(LogEntry(level: .success, message: "All missing dependencies installed successfully."))
            } else {
                self?.logs.append(LogEntry(level: .warning, message: "Some dependencies could not be installed."))
            }
        }
    }
    
    public func installDependency(_ dep: DependencyStatus) {
        showInstallerSheet = true
        isInstallingDependency = true
        installerLogs.removeAll()
        installerLogs.append("Installing \(dep.name)...")
        
        installer.install(dependency: dep) { [weak self] success in
            self?.isInstallingDependency = false
            self?.refreshPrerequisites()
            if success {
                self?.logs.append(LogEntry(level: .success, message: "\(dep.name) installed successfully."))
            } else {
                self?.logs.append(LogEntry(level: .error, message: "Failed to install \(dep.name)."))
            }
        }
    }
    
    public func repairDependency(_ dep: DependencyStatus) {
        showInstallerSheet = true
        isInstallingDependency = true
        installerLogs.removeAll()
        installerLogs.append("Repairing \(dep.name)...")
        
        installer.repair(dependency: dep) { [weak self] success in
            self?.isInstallingDependency = false
            self?.refreshPrerequisites()
            if success {
                self?.logs.append(LogEntry(level: .success, message: "\(dep.name) repaired successfully."))
            } else {
                self?.logs.append(LogEntry(level: .error, message: "Failed to repair \(dep.name)."))
            }
        }
    }
    
    public func confirmUninstall(_ dep: DependencyStatus) {
        self.dependencyToUninstall = dep
        self.showUninstallConfirm = true
    }
    
    public func executeUninstall() {
        guard let dep = dependencyToUninstall else { return }
        showUninstallConfirm = false
        showInstallerSheet = true
        isInstallingDependency = true
        installerLogs.removeAll()
        installerLogs.append("Uninstalling \(dep.name)...")
        
        installer.uninstall(dependency: dep) { [weak self] success in
            self?.isInstallingDependency = false
            self?.refreshPrerequisites()
            if success {
                self?.logs.append(LogEntry(level: .warning, message: "\(dep.name) was uninstalled."))
            } else {
                self?.logs.append(LogEntry(level: .error, message: "Failed to uninstall \(dep.name)."))
            }
        }
    }
    
    public func repairAllDependencies() {
        showInstallerSheet = true
        isInstallingDependency = true
        installerLogs.removeAll()
        installerLogs.append("Repairing all environment tools...")
        
        installer.repairAll(dependencies: prerequisites.list) { [weak self] success in
            self?.isInstallingDependency = false
            self?.refreshPrerequisites()
            if success {
                self?.logs.append(LogEntry(level: .success, message: "All environment tools repaired."))
            }
        }
    }
}
