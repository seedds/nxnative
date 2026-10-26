import Foundation
import SwiftUI
import AppKit

public final class AppViewModel: ObservableObject, BuildRunnerDelegate, InstallerServiceDelegate, UpdateCheckerDelegate {
    // MARK: - Storage Keys for Persistence
    private enum StorageKeys {
        static let savedGameFolder = "NXNative_SavedGameFolder"
        static let savedLibraryFolder = "NXNative_SavedLibraryFolder"
        static let savedCustomOutputFolder = "NXNative_SavedCustomOutputFolder"
        static let savedUseCustomOutput = "NXNative_SavedUseCustomOutput"
        static let savedProdKeysPath = "NXNative_SavedProdKeysPath"
        static let savedShowFps = "NXNative_SavedShowFps"
        static let savedKeepExtracted = "NXNative_SavedKeepExtracted"
        static let savedPlatform = "NXNative_SavedPlatform"
        static let savedIconAppearanceMode = "NXNative_SavedIconAppearanceMode"
    }
    
    // MARK: - Workspace & Paths
    @Published public var defaultInstallPath: String = ""
    @Published public var nxnativePath: String = ""
    @Published public var isNxnativeValid: Bool = false
    @Published public var isCloningRepository: Bool = false
    @Published public var cloneErrorMessage: String? = nil
    @Published public var enginePatches: [EnginePatchItem] = []
    @Published public var showCltInstallAlert: Bool = false
    
    // MARK: - Keys
    @Published public var prodKeysPath: String = "" {
        didSet {
            if !prodKeysPath.isEmpty {
                UserDefaults.standard.set(prodKeysPath, forKey: StorageKeys.savedProdKeysPath)
            }
        }
    }
    @Published public var prodKeysSource: String = ""
    @Published public var isProdKeysValid: Bool = false
    
    // MARK: - Game Selection
    @Published public var selectedGameFolder: URL? {
        didSet {
            if let folder = selectedGameFolder {
                UserDefaults.standard.set(folder.path, forKey: StorageKeys.savedGameFolder)
                scanGameFolder(folder)
            } else {
                UserDefaults.standard.removeObject(forKey: StorageKeys.savedGameFolder)
                detectedFiles = []
                detectedGames = []
                selectedGame = nil
            }
        }
    }
    @Published public var detectedFiles: [GameFileItem] = []
    @Published public var detectedGames: [SelectableGame] = []
    @Published public var selectedGame: SelectableGame? = nil {
        didSet {
            if let game = selectedGame {
                self.appName = game.name
            }
        }
    }
    
    // MARK: - Disk Capacity Pre-flight
    public var availableDiskBytes: Int64? {
        let checkPath = selectedGameFolder?.path ?? (defaultInstallPath.isEmpty ? NSHomeDirectory() : defaultInstallPath)
        var stat = statvfs()
        if statvfs(checkPath, &stat) == 0 {
            return Int64(stat.f_bavail) * Int64(stat.f_frsize)
        }
        let url = URL(fileURLWithPath: checkPath)
        let values = try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeAvailableCapacityKey])
        if let cap = values?.volumeAvailableCapacityForImportantUsage {
            return cap
        }
        return values?.volumeAvailableCapacity.map { Int64($0) }
    }
    
    public var isDiskSpaceLow: Bool {
        guard let avail = availableDiskBytes else { return false }
        let targetBytes: Int64
        if let sel = selectedGame {
            targetBytes = sel.totalSizeBytes
        } else if !detectedFiles.isEmpty {
            targetBytes = detectedFiles.reduce(0) { $0 + $1.sizeBytes }
        } else {
            return false
        }
        // Extraction requires at least the game size plus ~300MB buffer
        return avail < (targetBytes + 300_000_000)
    }
    
    public var diskSpaceWarningMessage: String? {
        guard isDiskSpaceLow, let avail = availableDiskBytes else { return nil }
        let targetBytes: Int64 = selectedGame?.totalSizeBytes ?? detectedFiles.reduce(0) { $0 + $1.sizeBytes }
        let needed = (targetBytes + 300_000_000) - avail
        let availStr = ByteCountFormatter.string(fromByteCount: avail, countStyle: .file)
        let totalStr = ByteCountFormatter.string(fromByteCount: targetBytes, countStyle: .file)
        let neededStr = ByteCountFormatter.string(fromByteCount: needed, countStyle: .file)
        let gameName = selectedGame?.name ?? "Game"
        return "Storage warning: \(gameName) extraction requires ~\(totalStr), but only \(availStr) is free on this volume. Please free up at least \(neededStr) to ensure the .app can be extracted and built."
    }
    
    // MARK: - Drag & Drop & ZIP
    @Published public var isExtractingZip: Bool = false
    @Published public var zipExtractionMessage: String = ""
    @Published public var dropErrorMessage: String? = nil
    
    // MARK: - Target & Output
    @Published public var platform: TargetPlatform = .macApp {
        didSet {
            UserDefaults.standard.set(platform.rawValue, forKey: StorageKeys.savedPlatform)
        }
    }
    @Published public var useCustomOutputFolder: Bool = false {
        didSet {
            UserDefaults.standard.set(useCustomOutputFolder, forKey: StorageKeys.savedUseCustomOutput)
        }
    }
    @Published public var customOutputFolder: URL? {
        didSet {
            if let folder = customOutputFolder {
                UserDefaults.standard.set(folder.path, forKey: StorageKeys.savedCustomOutputFolder)
            } else {
                UserDefaults.standard.removeObject(forKey: StorageKeys.savedCustomOutputFolder)
            }
        }
    }
    
    // MARK: - Library
    @Published public var libraryGames: [PortedGame] = []
    @Published public var libraryFolder: URL? {
        didSet {
            if let folder = libraryFolder {
                UserDefaults.standard.set(folder.path, forKey: StorageKeys.savedLibraryFolder)
            }
        }
    }
    @Published public var librarySearchText: String = ""
    @Published public var isScanningLibrary: Bool = false
    @Published public var gameForSettingsSheet: PortedGame? = nil
    
    // MARK: - Compatibility Database
    @Published public var compatibleGames: [CompatibleGameItem] = []
    @Published public var isLoadingCompatibility: Bool = false
    @Published public var compatibilityFilter: String = "All"
    @Published public var compatibilitySearchText: String = ""
    @Published public var compatibilityLastUpdated: Date? = nil
    @Published public var isCompatibilityFromOnline: Bool = false
    
    // MARK: - System Prerequisites & Installer
    @Published public var prerequisites: SystemPrerequisites = SystemPrerequisites()
    @Published public var isCheckingPrerequisites: Bool = false
    @Published public var isInstallingDependency: Bool = false
    @Published public var installerCurrentItem: String? = nil
    @Published public var installerLogs: [String] = []
    @Published public var showInstallerSheet: Bool = false
    @Published public var dependencyToUninstall: DependencyStatus? = nil
    @Published public var showUninstallConfirm: Bool = false
    
    // MARK: - Update Checker
    @Published public var isCheckingUpdate: Bool = false
    @Published public var updateInfo: UpdateInfo? = nil
    @Published public var isApplyingUpdate: Bool = false
    @Published public var updateSuccessBanner: String? = nil
    
    // MARK: - Advanced Options
    @Published public var appName: String = ""
    @Published public var bundleId: String = ""
    @Published public var selectedLanguage: String = "auto"
    @Published public var showFps: Bool = true {
        didSet {
            UserDefaults.standard.set(showFps, forKey: StorageKeys.savedShowFps)
        }
    }
    @Published public var keepExtracted: Bool = true {
        didSet {
            UserDefaults.standard.set(keepExtracted, forKey: StorageKeys.savedKeepExtracted)
        }
    }
    @Published public var surveyOnly: Bool = false
    
    // MARK: - Build State
    @Published public var isBuilding: Bool = false
    @Published public var currentStageName: String = ""
    @Published public var currentStepName: String = ""
    @Published public var stageIndex: Int = 0
    @Published public var totalStages: Int = 0
    @Published public var progressFraction: Double = 0.0
    @Published public var etaSeconds: Double? = nil
    @Published public var buildStages: [BuildStageItem] = []
    @Published public var showLiveLogsInBuild: Bool = true
    @Published public var logs: [LogEntry] = []
    @Published public var buildError: String? = nil
    @Published public var buildSuccessPath: String? = nil
    @Published public var buildWizardStep: Int = 1 // 1: Select Game, 2: Configure & Output, 3: Porting
    @Published public var iconAppearanceMode: AppIconAppearanceMode = .auto {
        didSet {
            UserDefaults.standard.set(iconAppearanceMode.rawValue, forKey: StorageKeys.savedIconAppearanceMode)
            updateDockIcon()
        }
    }
    
    // MARK: - Navigation
    @Published public var selectedTab: Int = 0 // 0: Convert, 1: Library, 2: Settings, 3: Console
    
    public let availableLanguages: [(code: String, label: String)] = [
        ("auto", "Game Default"),
        ("en-US", "English (US)"),
        ("en-GB", "English (UK)"),
        ("ja", "Japanese"),
        ("pt-BR", "Portuguese (Brazil)"),
        ("es", "Spanish"),
        ("fr", "French"),
        ("de", "German"),
        ("it", "Italian"),
        ("zh-Hans", "Chinese (Simplified)"),
        ("ko", "Korean")
    ]
    
    private let runner = BuildRunner()
    private let installer = InstallerService.shared
    private let updateChecker = UpdateCheckerService.shared
    
    public init() {
        runner.delegate = self
        installer.delegate = self
        updateChecker.delegate = self
        
        // 1. Resolve safe root directory (anchored to SSD / Bundle, NEVER "/")
        let root = SystemDetector.shared.resolveRootDirectory()
        self.defaultInstallPath = root.path
        
        // Restore saved library folder or default to root/Games
        if let savedLib = UserDefaults.standard.string(forKey: StorageKeys.savedLibraryFolder),
           FileManager.default.fileExists(atPath: savedLib) {
            self.libraryFolder = URL(fileURLWithPath: savedLib)
        } else {
            self.libraryFolder = root.appendingPathComponent("Games")
        }
        
        // Restore custom output settings
        if UserDefaults.standard.object(forKey: StorageKeys.savedUseCustomOutput) != nil {
            self.useCustomOutputFolder = UserDefaults.standard.bool(forKey: StorageKeys.savedUseCustomOutput)
        }
        if let savedOut = UserDefaults.standard.string(forKey: StorageKeys.savedCustomOutputFolder),
           FileManager.default.fileExists(atPath: savedOut) {
            self.customOutputFolder = URL(fileURLWithPath: savedOut)
        }
        
        // Restore user preferences
        if let savedFps = UserDefaults.standard.object(forKey: StorageKeys.savedShowFps) as? Bool {
            self.showFps = savedFps
        }
        if let savedKeep = UserDefaults.standard.object(forKey: StorageKeys.savedKeepExtracted) as? Bool {
            self.keepExtracted = savedKeep
        }
        if let savedPlat = UserDefaults.standard.string(forKey: StorageKeys.savedPlatform),
           let p = TargetPlatform(rawValue: savedPlat) {
            self.platform = p
        }
        if let savedMode = UserDefaults.standard.string(forKey: StorageKeys.savedIconAppearanceMode),
           let mode = AppIconAppearanceMode(rawValue: savedMode) {
            self.iconAppearanceMode = mode
        }
        
        detectEnvironment()
        
        // Restore saved game folder if it exists
        if let savedGamePath = UserDefaults.standard.string(forKey: StorageKeys.savedGameFolder) {
            var isDir: ObjCBool = false
            if FileManager.default.fileExists(atPath: savedGamePath, isDirectory: &isDir), isDir.boolValue {
                self.selectedGameFolder = URL(fileURLWithPath: savedGamePath)
            }
        }
        
        refreshLibrary()
        checkForUpdates()
        loadCompatibleGames()
        updateDockIcon()
    }
    
    // MARK: - Dynamic Dock Icon
    public func updateDockIcon() {
        let isDark = NSApp?.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        let resName: String
        switch iconAppearanceMode {
        case .light: resName = "AppIcon-Light"
        case .dark: resName = "AppIcon-Dark"
        case .mono: resName = "AppIcon-Mono"
        case .auto: resName = isDark ? "AppIcon-Dark" : "AppIcon-Light"
        }
        
        if let url = Bundle.main.url(forResource: resName, withExtension: "png"),
           let img = NSImage(contentsOf: url) {
            NSApplication.shared.applicationIconImage = img
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
    
    // MARK: - Drag and Drop & ZIP
    public func handleDroppedURLs(_ urls: [URL]) {
        dropErrorMessage = nil
        guard let first = urls.first else { return }
        
        var isDir: ObjCBool = false
        if FileManager.default.fileExists(atPath: first.path, isDirectory: &isDir) {
            if isDir.boolValue {
                self.selectedGameFolder = first
            } else if first.pathExtension.lowercased() == "zip" {
                extractDroppedZip(zipURL: first)
            } else {
                let parent = first.deletingLastPathComponent()
                self.selectedGameFolder = parent
            }
        }
    }
    
    private func extractDroppedZip(zipURL: URL) {
        let gamesDir = URL(fileURLWithPath: defaultInstallPath).appendingPathComponent("Games")
        isExtractingZip = true
        zipExtractionMessage = "Analyzing ZIP archive..."
        
        ZipExtractor.shared.extract(
            zipURL: zipURL,
            destinationFolder: gamesDir,
            progressCallback: { [weak self] msg in
                self?.zipExtractionMessage = msg
            },
            completion: { [weak self] result in
                guard let self = self else { return }
                self.isExtractingZip = false
                
                switch result {
                case .success(let extractedDir):
                    self.selectedGameFolder = extractedDir
                    self.logs.append(LogEntry(level: .success, message: "Archive extracted to: \(extractedDir.path)"))
                case .failure(let error):
                    self.dropErrorMessage = error.localizedDescription
                    self.logs.append(LogEntry(level: .error, message: "Failed to extract archive: \(error.localizedDescription)"))
                }
            }
        )
    }
    
    // MARK: - Library Operations (Privacy-guarded)
    public func refreshLibrary() {
        isScanningLibrary = true
        var searchDirs: [URL] = []
        
        let root = URL(fileURLWithPath: defaultInstallPath)
        let gamesDir = root.appendingPathComponent("Games")
        searchDirs.append(gamesDir)
        
        if let custom = libraryFolder, custom.path != "/" && custom.path != gamesDir.path {
            searchDirs.append(custom)
        }
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let games = LibraryScanner.shared.scan(directories: searchDirs)
            DispatchQueue.main.async {
                self?.libraryGames = games
                self?.isScanningLibrary = false
            }
        }
    }
    
    public func selectLibraryFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Select Library Folder"
        panel.message = "Choose a folder containing your built Mac Apps or IPAs."
        
        if panel.runModal() == .OK, let url = panel.url {
            self.libraryFolder = url
            refreshLibrary()
        }
    }
    
    public func launchGame(_ game: PortedGame) {
        let config = GameRuntimeConfig.load(for: game)
        config.applyToDisk(for: game)
        
        LibraryScanner.shared.launchGame(at: game.url, env: config.toEnvironmentDict()) { [weak self] success, error in
            if success {
                self?.logs.append(LogEntry(level: .success, message: "Launched game: \(game.name)"))
            } else if let err = error {
                self?.logs.append(LogEntry(level: .error, message: "Failed to launch game: \(err)"))
            }
        }
    }
    
    public func loadRuntimeConfig(for game: PortedGame) -> GameRuntimeConfig {
        GameRuntimeConfig.load(for: game)
    }
    
    public func saveRuntimeConfig(_ config: GameRuntimeConfig, for game: PortedGame) {
        config.save(for: game)
        logs.append(LogEntry(level: .success, message: "Saved settings for \(game.name)"))
    }
    
    public func revealInFinder(url: URL) {
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
    
    public func deletePortedGame(_ game: PortedGame) {
        do {
            try FileManager.default.removeItem(at: game.url)
            logs.append(LogEntry(level: .warning, message: "Game removed: \(game.name)"))
            refreshLibrary()
        } catch {
            logs.append(LogEntry(level: .error, message: "Failed to delete game: \(error.localizedDescription)"))
        }
    }
    
    // MARK: - GitHub Updates
    public func checkForUpdates() {
        guard isNxnativeValid, !nxnativePath.isEmpty else { return }
        isCheckingUpdate = true
        updateChecker.checkForUpdates(nxnativePath: nxnativePath) { [weak self] info in
            self?.isCheckingUpdate = false
            self?.updateInfo = info
        }
    }
    
    public func applyUpdate() {
        guard isNxnativeValid, !nxnativePath.isEmpty else { return }
        isApplyingUpdate = true
        updateChecker.applyUpdate(nxnativePath: nxnativePath) { [weak self] success, message in
            self?.isApplyingUpdate = false
            if success {
                self?.updateSuccessBanner = message
                self?.updateInfo = nil
                self?.refreshPrerequisites()
                self?.logs.append(LogEntry(level: .success, message: message))
            } else {
                self?.logs.append(LogEntry(level: .error, message: "Update error: \(message)"))
            }
        }
    }
    
    // MARK: - Game Scan
    private func scanGameFolder(_ folder: URL) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let files = GameScanner.shared.scan(directory: folder)
            let games = GameScanner.shared.groupFilesIntoGames(files: files)
            DispatchQueue.main.async {
                self?.detectedFiles = files
                self?.detectedGames = games
                self?.selectedGame = games.first
                
                if let self = self, let firstGame = games.first {
                    self.appName = firstGame.name
                }
            }
        }
    }
    
    // MARK: - File Pickers
    public func selectNxnativeFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Select nxnative Folder"
        panel.message = "Choose the directory containing the nxnative scripts and tools."
        
        if panel.runModal() == .OK, let url = panel.url {
            let res = SystemDetector.shared.detectNxnativeDirectory(customDir: url.path)
            self.nxnativePath = res.path
            self.isNxnativeValid = res.isValid
            if res.isValid {
                refreshPrerequisites()
                checkForUpdates()
            }
        }
    }
    
    public func selectGameFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose Game Folder"
        panel.message = "Select a folder holding your game NSP, NSZ, or XCI files."
        
        if panel.runModal() == .OK, let url = panel.url {
            self.selectedGameFolder = url
        }
    }
    
    public func selectOutputFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose Output Folder"
        panel.message = "Select the folder where the IPA or Mac App will be saved."
        
        if panel.runModal() == .OK, let url = panel.url {
            self.customOutputFolder = url
        }
    }
    
    public func selectProdKeysFile() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = []
        panel.prompt = "Select prod.keys"
        panel.message = "Choose your Nintendo Switch prod.keys file."
        
        if panel.runModal() == .OK, let url = panel.url {
            self.prodKeysPath = url.path
            self.prodKeysSource = "User Selected"
            self.isProdKeysValid = true
        }
    }
    
    public func selectRyujinxFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Select Ryujinx Folder"
        panel.message = "Select your Ryujinx folder to automatically locate prod.keys."
        
        if panel.runModal() == .OK, let url = panel.url {
            detectEnvironment(customRyujinxDir: url.path)
        }
    }
    
    // MARK: - Build Actions
    public var canStartBuild: Bool {
        !isBuilding &&
        selectedGameFolder != nil &&
        selectedGame != nil &&
        isProdKeysValid &&
        isNxnativeValid &&
        !isDiskSpaceLow
    }
    
    public func goToWizardStep(_ step: Int) {
        guard step >= 1 && step <= 3 else { return }
        if step == 2 && selectedGame == nil { return }
        buildWizardStep = step
    }
    
    public func resetWizard() {
        buildWizardStep = 1
        buildError = nil
        buildSuccessPath = nil
    }
    
    public func startBuild() {
        guard canStartBuild, let gameDir = selectedGameFolder, let sel = selectedGame else { return }
        
        buildWizardStep = 3
        isBuilding = true
        buildError = nil
        buildSuccessPath = nil
        progressFraction = 0.0
        currentStageName = "Preparing Build..."
        currentStepName = ""
        stageIndex = 0
        totalStages = 0
        etaSeconds = nil
        buildStages.removeAll()
        logs.removeAll()
        
        // Staging: isolate selected game files via symlinks if multiple games exist in folder or files are shared
        var targetGameDirectory = gameDir
        if detectedGames.count > 1 || sel.files.count < detectedFiles.count {
            let safeName = sel.name.replacingOccurrences(of: "[^a-zA-Z0-9._-]", with: "_", options: .regularExpression)
            let stagingDir = URL(fileURLWithPath: defaultInstallPath)
                .appendingPathComponent("Staging")
                .appendingPathComponent(safeName)
            
            let fm = FileManager.default
            if fm.fileExists(atPath: stagingDir.path) {
                try? fm.removeItem(at: stagingDir)
            }
            do {
                try fm.createDirectory(at: stagingDir, withIntermediateDirectories: true)
                for item in sel.files {
                    let linkURL = stagingDir.appendingPathComponent(item.url.lastPathComponent)
                    try fm.createSymbolicLink(at: linkURL, withDestinationURL: item.url)
                }
                targetGameDirectory = stagingDir
                logs.append(LogEntry(level: .info, message: "Isolated '\(sel.name)' (\(sel.files.count) files) in staging environment."))
            } catch {
                logs.append(LogEntry(level: .warning, message: "Could not create staging symlinks (\(error.localizedDescription)). Using original directory."))
            }
        }
        
        let targetOutDir = useCustomOutputFolder ? customOutputFolder : URL(fileURLWithPath: defaultInstallPath).appendingPathComponent("Games")
        
        runner.startBuild(
            nxnativePath: nxnativePath,
            gameDirectory: targetGameDirectory,
            prodKeysPath: prodKeysPath,
            platform: platform,
            customOutputFolder: targetOutDir,
            appName: appName.isEmpty ? sel.name : appName,
            bundleId: bundleId.isEmpty ? nil : bundleId,
            language: selectedLanguage,
            showFps: showFps,
            keepExtracted: keepExtracted,
            surveyOnly: surveyOnly
        )
    }
    
    public func cancelBuild() {
        runner.cancel()
    }
    
    public func openOutputInFinder() {
        guard let path = buildSuccessPath else { return }
        let url = URL(fileURLWithPath: path)
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
    
    // MARK: - Delegates
    public func buildRunner(_ runner: BuildRunner, didProduceLog entry: LogEntry) {
        logs.append(entry)
    }
    
    public func buildRunner(_ runner: BuildRunner, didUpdateStage stage: String, index: Int, total: Int) {
        self.currentStageName = stage
        self.stageIndex = index
        self.totalStages = total
        
        // Mark any previous running stages as completed
        for i in 0..<buildStages.count {
            if buildStages[i].id < index && buildStages[i].isRunning {
                buildStages[i].isRunning = false
                buildStages[i].isCompleted = true
            }
        }
        
        if let existingIdx = buildStages.firstIndex(where: { $0.id == index }) {
            buildStages[existingIdx].isRunning = true
            buildStages[existingIdx].step = ""
        } else {
            buildStages.append(BuildStageItem(id: index, name: stage, isRunning: true))
        }
    }
    
    public func buildRunner(_ runner: BuildRunner, didUpdateStep step: String) {
        self.currentStepName = step
        if let idx = buildStages.firstIndex(where: { $0.id == stageIndex }) {
            buildStages[idx].step = step
        }
    }
    
    public func buildRunner(_ runner: BuildRunner, didUpdateProgress fraction: Double, doneBytes: Double?, totalBytes: Double?, etaSeconds: Double?) {
        self.progressFraction = fraction
        self.etaSeconds = etaSeconds
    }
    
    public func buildRunner(_ runner: BuildRunner, didEndStage stage: String, index: Int, ok: Bool, elapsed: Double?, summary: String?) {
        if let idx = buildStages.firstIndex(where: { $0.id == index }) {
            buildStages[idx].isRunning = false
            buildStages[idx].isCompleted = ok
            buildStages[idx].isFailed = !ok
            buildStages[idx].elapsedSeconds = elapsed
            buildStages[idx].summary = summary
        }
    }
    
    public func buildRunner(_ runner: BuildRunner, didFinishSuccess outputPath: String?) {
        self.isBuilding = false
        self.progressFraction = 1.0
        self.currentStageName = "Build Completed!"
        for i in 0..<buildStages.count {
            buildStages[i].isRunning = false
            buildStages[i].isCompleted = true
        }
        self.buildSuccessPath = outputPath
        self.logs.append(LogEntry(level: .success, message: "Build succeeded! Output: \(outputPath ?? "Unknown")"))
        refreshLibrary()
    }
    
    public func buildRunner(_ runner: BuildRunner, didFinishWithError error: String) {
        self.isBuilding = false
        if let idx = buildStages.firstIndex(where: { $0.isRunning }) {
            buildStages[idx].isRunning = false
            buildStages[idx].isFailed = true
        }
        self.buildError = error
        self.logs.append(LogEntry(level: .error, message: error))
    }
    
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
