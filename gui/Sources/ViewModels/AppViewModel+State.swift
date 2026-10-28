import Foundation

extension AppViewModel {
    public static let availableLanguages: [(code: String, label: String)] = [
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

    public var appOutputDirectory: URL {
        if useCustomOutputFolder, let custom = customOutputFolder {
            return custom
        }
        if !defaultInstallPath.isEmpty {
            return URL(fileURLWithPath: defaultInstallPath).appendingPathComponent("Games")
        }
        return URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Applications")
    }

    public var availableDiskBytes: Int64? {
        var current = appOutputDirectory
        while !FileManager.default.fileExists(atPath: current.path) && current.pathComponents.count > 1 {
            current = current.deletingLastPathComponent()
        }
        let checkPath = current.path
        var stat = statvfs()
        if statvfs(checkPath, &stat) == 0 {
            return Int64(stat.f_bavail) * Int64(stat.f_frsize)
        }
        let values = try? current.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeAvailableCapacityKey])
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
        return avail < (targetBytes + 300_000_000)
    }

    // MARK: - Modal Presentation & Navigation Guard
    public var isAnyModalPresented: Bool {
        gameForSettingsSheet != nil ||
        isGameActionMenuPresented ||
        showAboutSheet ||
        showUpdateSheet ||
        showInstallerSheet ||
        showDeleteConfirm ||
        showRemoveConfirm ||
        showTrashConfirm
    }

    public var hasHiddenGames: Bool {
        !hiddenGamePaths.isEmpty
    }

    public var hiddenGamesCount: Int {
        hiddenGamePaths.count
    }

    public var macLibraryGames: [PortedGame] {
        libraryGames.filter { $0.platform == .macApp }
    }

    public var iosLibraryGames: [PortedGame] {
        libraryGames.filter { $0.platform == .ios }
    }

    public var diskSpaceWarningMessage: String? {
        guard isDiskSpaceLow, let avail = availableDiskBytes else { return nil }
        let targetBytes: Int64 = selectedGame?.totalSizeBytes ?? detectedFiles.reduce(0) { $0 + $1.sizeBytes }
        let needed = (targetBytes + 300_000_000) - avail
        let availStr = ByteCountFormatter.string(fromByteCount: avail, countStyle: .file)
        let totalStr = ByteCountFormatter.string(fromByteCount: targetBytes, countStyle: .file)
        let neededStr = ByteCountFormatter.string(fromByteCount: needed, countStyle: .file)
        let gameName = selectedGame?.name ?? "Game"
        return "Storage warning: \(gameName) extraction requires ~\(totalStr), but only \(availStr) is free on output destination. Please free up at least \(neededStr) on that volume to ensure the .app can be built."
    }
}
