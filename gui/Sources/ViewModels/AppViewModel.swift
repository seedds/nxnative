import Foundation
import SwiftUI
import AppKit

public final class AppViewModel: ObservableObject, BuildRunnerDelegate, InstallerServiceDelegate, UpdateCheckerDelegate {
    // MARK: - Storage Keys for Persistence
    public enum StorageKeys {
        public static let savedGameFolder = "NXNative_SavedGameFolder"
        public static let savedLibraryFolder = "NXNative_SavedLibraryFolder"
        public static let savedCustomOutputFolder = "NXNative_SavedCustomOutputFolder"
        public static let savedUseCustomOutput = "NXNative_SavedUseCustomOutput"
        public static let savedProdKeysPath = "NXNative_SavedProdKeysPath"
        public static let savedShowFps = "NXNative_SavedShowFps"
        public static let savedKeepExtracted = "NXNative_SavedKeepExtracted"
        public static let savedPlatform = "NXNative_SavedPlatform"
        public static let savedIconAppearanceMode = "NXNative_SavedIconAppearanceMode_v2"
        public static let savedHighlightTheme = "NXNative_SavedHighlightTheme"
        public static let savedCompatibilityDisplayMode = "NXNative_SavedCompatibilityDisplayMode"
        public static let savedLibraryCardSize = "NXNative_SavedLibraryCardSize"
        public static let savedLibraryCardScale = "NXNative_SavedLibraryCardScale"
        public static let savedLauncherLanguage = "NXNative_SavedLauncherLanguage"
        public static let savedHiddenGamePaths = "NXNative_SavedHiddenGamePaths"
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
    
    // MARK: - Library State & Card Scale Slider
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
    @Published public var libraryCardSize: LibraryCardSize = .regular {
        didSet {
            UserDefaults.standard.set(libraryCardSize.rawValue, forKey: StorageKeys.savedLibraryCardSize)
        }
    }
    @Published public var libraryCardScale: Double = 1.0 {
        didSet {
            UserDefaults.standard.set(libraryCardScale, forKey: StorageKeys.savedLibraryCardScale)
        }
    }
    @Published public var gameForSettingsSheet: PortedGame? = nil {
        didSet {
            if let game = gameForSettingsSheet {
                activeSettingsConfig = GameRuntimeConfig.load(for: game)
                settingsModalFocusIndex = 0
            }
        }
    }
    @Published public var activeSettingsConfig: GameRuntimeConfig = GameRuntimeConfig()
    @Published public var settingsModalFocusIndex: Int = 0
    @Published public var gameToDelete: PortedGame? = nil
    @Published public var showDeleteConfirm: Bool = false
    @Published public var hiddenGamePaths: Set<String> = [] {
        didSet {
            UserDefaults.standard.set(Array(hiddenGamePaths), forKey: StorageKeys.savedHiddenGamePaths)
        }
    }
    @Published public var gameToRemove: PortedGame? = nil
    @Published public var showRemoveConfirm: Bool = false
    @Published public var gameToTrash: PortedGame? = nil
    @Published public var showTrashConfirm: Bool = false
    
    // MARK: - Compatibility Database
    @Published public var compatibleGames: [CompatibleGameItem] = []
    @Published public var isLoadingCompatibility: Bool = false
    @Published public var compatibilityFilter: String = "All"
    @Published public var compatibilityRamFilter: Int? = nil
    @Published public var compatibilityNameFilter: String = "All"
    @Published public var compatibilitySearchText: String = ""
    @Published public var compatibilityLastUpdated: Date? = nil
    @Published public var isCompatibilityFromOnline: Bool = false
    @Published public var compatibilityDisplayMode: CompatibilityDisplayMode = .list {
        didSet {
            UserDefaults.standard.set(compatibilityDisplayMode.rawValue, forKey: StorageKeys.savedCompatibilityDisplayMode)
        }
    }
    @Published public var compatibilityFocusArea: CompatibilityFocusArea = .filters
    @Published public var focusedCompatibilityIndex: Int = 0
    @Published public var focusedCompatibilityButtonIndex: CompatibilityButtonFocus = .notes
    
    // MARK: - System Prerequisites & Installer
    @Published public var prerequisites: SystemPrerequisites = SystemPrerequisites()
    @Published public var isCheckingPrerequisites: Bool = false
    @Published public var isInstallingDependency: Bool = false
    @Published public var installerCurrentItem: String? = nil
    @Published public var installerLogs: [String] = []
    @Published public var showInstallerSheet: Bool = false
    @Published public var dependencyToUninstall: DependencyStatus? = nil
    @Published public var showUninstallConfirm: Bool = false
    
    // MARK: - Update Checker & About Sheet
    @Published public var showAboutSheet: Bool = false
    @Published public var showUpdateSheet: Bool = false
    @Published public var isCheckingUpdate: Bool = false
    @Published public var updateInfo: UpdateInfo? = nil
    @Published public var isApplyingUpdate: Bool = false
    @Published public var updateInstallLogs: [String] = []
    @Published public var updateCompletedReadyToRelaunch: Bool = false
    @Published public var updateSuccessBanner: String? = nil
    @Published public var updateErrorBanner: String? = nil
    
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
    @Published public var buildWizardStep: Int = 1
    @Published public var iconAppearanceMode: AppIconAppearanceMode = .light {
        didSet {
            UserDefaults.standard.set(iconAppearanceMode.rawValue, forKey: StorageKeys.savedIconAppearanceMode)
            updateDockIcon()
        }
    }
    @Published public var highlightTheme: LauncherHighlightTheme = NXTheme.activeHighlightTheme {
        didSet {
            NXTheme.activeHighlightTheme = highlightTheme
            UserDefaults.standard.set(highlightTheme.rawValue, forKey: StorageKeys.savedHighlightTheme)
        }
    }
    @Published public var launcherLanguage: AppLanguage = .system {
        didSet {
            UserDefaults.standard.set(launcherLanguage.rawValue, forKey: StorageKeys.savedLauncherLanguage)
            LocalizationService.shared.currentLanguage = launcherLanguage
        }
    }
    
    // MARK: - Navigation & Gamepad State
    @Published public var selectedTab: Int = 0
    @Published public var focusedGameIndex: Int = 0
    @Published public var isFocusOnDeck: Bool = false
    @Published public var isGameActionMenuPresented: Bool = false
    @Published public var gameActionFocusedIndex: Int = 0
    @Published public var launchingGameId: UUID? = nil
    
    public var availableLanguages: [(code: String, label: String)] {
        Self.availableLanguages
    }
    
    public func tr(_ key: String, _ args: CVarArg...) -> String {
        LocalizationService.shared.format(key: key, arguments: args)
    }
    
    let runner = BuildRunner()
    let installer = InstallerService.shared
    let updateChecker = UpdateCheckerService.shared
    
    public init() {
        runner.delegate = self
        installer.delegate = self
        updateChecker.delegate = self
        
        let root = SystemDetector.shared.resolveRootDirectory()
        self.defaultInstallPath = root.path
        
        if let savedLib = UserDefaults.standard.string(forKey: StorageKeys.savedLibraryFolder),
           FileManager.default.fileExists(atPath: savedLib) {
            self.libraryFolder = URL(fileURLWithPath: savedLib)
        } else {
            self.libraryFolder = root.appendingPathComponent("Games")
        }
        
        if UserDefaults.standard.object(forKey: StorageKeys.savedUseCustomOutput) != nil {
            self.useCustomOutputFolder = UserDefaults.standard.bool(forKey: StorageKeys.savedUseCustomOutput)
        }
        if let savedOut = UserDefaults.standard.string(forKey: StorageKeys.savedCustomOutputFolder),
           FileManager.default.fileExists(atPath: savedOut) {
            self.customOutputFolder = URL(fileURLWithPath: savedOut)
        }
        
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
        if let savedTheme = UserDefaults.standard.string(forKey: StorageKeys.savedHighlightTheme),
           let theme = LauncherHighlightTheme(rawValue: savedTheme) {
            NXTheme.activeHighlightTheme = theme
            self.highlightTheme = theme
        }
        if let savedDisplay = UserDefaults.standard.string(forKey: StorageKeys.savedCompatibilityDisplayMode),
           let dispMode = CompatibilityDisplayMode(rawValue: savedDisplay) {
            self.compatibilityDisplayMode = dispMode
        }
        if let savedCardSize = UserDefaults.standard.string(forKey: StorageKeys.savedLibraryCardSize),
           let size = LibraryCardSize(rawValue: savedCardSize) {
            self.libraryCardSize = size
        }
        if let savedScale = UserDefaults.standard.object(forKey: StorageKeys.savedLibraryCardScale) as? Double {
            self.libraryCardScale = max(0.70, min(1.35, savedScale))
        }
        if let savedLang = UserDefaults.standard.string(forKey: StorageKeys.savedLauncherLanguage),
           let lang = AppLanguage(rawValue: savedLang) {
            self.launcherLanguage = lang
            LocalizationService.shared.currentLanguage = lang
        } else {
            self.launcherLanguage = .system
            LocalizationService.shared.currentLanguage = .system
        }
        
        detectEnvironment()
        
        if let savedGamePath = UserDefaults.standard.string(forKey: StorageKeys.savedGameFolder) {
            var isDir: ObjCBool = false
            if FileManager.default.fileExists(atPath: savedGamePath, isDirectory: &isDir), isDir.boolValue {
                self.selectedGameFolder = URL(fileURLWithPath: savedGamePath)
            }
        }
        
        if let savedHidden = UserDefaults.standard.stringArray(forKey: StorageKeys.savedHiddenGamePaths) {
            self.hiddenGamePaths = Set(savedHidden)
        }
        
        refreshLibrary()
        checkForUpdates()
        loadCompatibleGames()
        updateDockIcon()
        setupWindowMinimizeIconObserver()
        
        NotificationService.shared.requestAuthorization()
        setupControllerNavigation()
    }
}
