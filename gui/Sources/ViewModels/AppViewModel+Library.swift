import Foundation
import AppKit

extension AppViewModel {
    // MARK: - Library Operations (Privacy-guarded)
    public func refreshLibrary() {
        isScanningLibrary = true
        var searchDirs: [URL] = []
        
        let root = URL(fileURLWithPath: defaultInstallPath)
        let gamesDir = root.appendingPathComponent("Games")
        searchDirs.append(gamesDir)

        let nxPortsDir = URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/Nx Ports")
        if FileManager.default.fileExists(atPath: nxPortsDir.path) {
            searchDirs.append(nxPortsDir)
        }
        
        if let custom = libraryFolder, custom.path != "/" && custom.path != gamesDir.path {
            searchDirs.append(custom)
        }
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let games = LibraryScanner.shared.scan(directories: searchDirs)
            DispatchQueue.main.async {
                guard let self = self else { return }
                let visible = LibraryOrder.apply(to: games.filter { !self.hiddenGamePaths.contains($0.url.standardized.path) })
                self.libraryGames = visible
                if self.focusedGameIndex >= visible.count {
                    self.focusedGameIndex = max(0, visible.count - 1)
                }
                self.isScanningLibrary = false
                
                // Automatically ensure every game in the library has its correct official cover and input mapping
                DispatchQueue.global(qos: .utility).async {
                    InputProfileManager.shared.exportActiveMappingToDisk(libraryFolder: self.libraryFolder)
                    for game in visible {
                        GameCoverService.shared.ensureCover(for: game)
                    }
                }
            }
        }
    }
    
    /// Moves `dragged` to the position of `target` inside the same shelf and persists the new order.
    public func moveLibraryGame(_ dragged: PortedGame, onto target: PortedGame) {
        guard dragged.id != target.id, dragged.platform == target.platform,
              let from = libraryGames.firstIndex(where: { $0.id == dragged.id }),
              let to = libraryGames.firstIndex(where: { $0.id == target.id }) else { return }
        let focusedId = libraryGames.indices.contains(focusedGameIndex) ? libraryGames[focusedGameIndex].id : nil
        var games = libraryGames
        games.insert(games.remove(at: from), at: to)
        libraryGames = games
        if let focusedId, let idx = games.firstIndex(where: { $0.id == focusedId }) {
            focusedGameIndex = idx
        }
        LibraryOrder.save(games)
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
        guard launchingGameId == nil else { return }
        launchingGameId = game.id
        SoundEffectService.shared.play(.launch)
        
        let config = GameRuntimeConfig.load(for: game)
        config.applyToDisk(for: game)
        InputProfileManager.shared.exportActiveMappingToDisk(libraryFolder: libraryFolder)
        
        // Ensure iCloud Cross-Save is configured with cover art if iCloud Drive is available
        var savePath: String? = nil
        if SaveSyncService.shared.isICloudDriveAvailable {
            let cover = GameCoverService.shared.coverImage(for: game)
            if case .success(let cloudSaveDir) = SaveSyncService.shared.ensureCloudSaveConfigured(for: game, cover: cover) {
                savePath = cloudSaveDir.path
            }
        }
        
        // Auto-sync saves with connected device before launch if available
        SaveSyncService.shared.autoSyncOnLaunch(for: game) { [weak self] status, msg in
            if let status = status, status != .noSaves && status != .alreadyInSync {
                self?.logs.append(LogEntry(level: .info, message: "Auto-sync on launch: \(msg)"))
            }
            var env = config.toEnvironmentDict(savePath: savePath)
            let prof = InputProfileManager.shared.activeProfile
            env["DM_MOUSE_RIGHT_STICK"] = prof.mouseAsRightStick ? "1" : "0"
            env["DM_MOUSE_SENSITIVITY"] = String(format: "%.2f", locale: Locale(identifier: "en_US_POSIX"), prof.mouseSensitivity)
            env["DM_MOUSE_INVERT_X"] = prof.mouseInvertX ? "1" : "0"
            env["DM_MOUSE_INVERT_Y"] = prof.mouseInvertY ? "1" : "0"
            LibraryScanner.shared.launchGame(at: game.url, env: env) { [weak self] success, error in
                DispatchQueue.main.async {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.8) {
                        self?.launchingGameId = nil
                    }
                    if success {
                        self?.runningGameTitle = game.name
                        self?.logs.append(LogEntry(level: .success, message: "Launched game: \(game.name)"))
                        MemoryOptimizer.shared.optimizeForBackground(viewModel: self)
                        
                        // Observe game termination to clear runningGameTitle
                        if let runningApp = GameControllerService.shared.activeRunningGameApp {
                            NotificationCenter.default.addObserver(
                                forName: NSWorkspace.didTerminateApplicationNotification,
                                object: nil,
                                queue: .main
                            ) { [weak self] notif in
                                if let termApp = notif.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication,
                                   termApp.processIdentifier == runningApp.processIdentifier {
                                    self?.runningGameTitle = nil
                                }
                            }
                        }
                    } else if let err = error {
                        self?.runningGameTitle = nil
                        SoundEffectService.shared.play(.error)
                        self?.logs.append(LogEntry(level: .error, message: "Failed to launch game: \(err)"))
                    }
                }
            }
        }
    }
    
    public func loadRuntimeConfig(for game: PortedGame) -> GameRuntimeConfig {
        let key = game.uniqueKey
        if let cached = runtimeConfigCache[key] {
            return cached
        }
        let loaded = GameRuntimeConfig.load(for: game)
        runtimeConfigCache[key] = loaded
        return loaded
    }
    
    public func saveRuntimeConfig(_ config: GameRuntimeConfig, for game: PortedGame) {
        runtimeConfigCache[game.uniqueKey] = config
        config.save(for: game)
        logs.append(LogEntry(level: .success, message: "Saved settings for \(game.name)"))
    }
    
    public func revealInFinder(url: URL) {
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
    
    public func revealSaveFolder(_ url: URL) {
        let fm = FileManager.default
        if !fm.fileExists(atPath: url.path) {
            try? fm.createDirectory(at: url, withIntermediateDirectories: true)
        }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
    
    public func removeGameFromLibrary(_ game: PortedGame) {
        hiddenGamePaths.insert(game.url.standardized.path)
        SoundEffectService.shared.play(.click)
        logs.append(LogEntry(level: .warning, message: "Removed from library: \(game.name) (files preserved on disk)"))
        refreshLibrary()
    }

    public func restoreHiddenGame(path: String) {
        hiddenGamePaths.remove(path)
        SoundEffectService.shared.play(.click)
        logs.append(LogEntry(level: .success, message: "Restored game to library"))
        refreshLibrary()
    }

    public func restoreAllHiddenGames() {
        let count = hiddenGamePaths.count
        hiddenGamePaths.removeAll()
        SoundEffectService.shared.play(.click)
        logs.append(LogEntry(level: .success, message: "Restored \(count) hidden games to library"))
        refreshLibrary()
    }

    public func deleteGameToTrash(_ game: PortedGame) {
        do {
            var resultingURL: NSURL?
            try FileManager.default.trashItem(at: game.url, resultingItemURL: &resultingURL)
            hiddenGamePaths.remove(game.url.standardized.path)
            ArtworkStore.shared.clearCache(for: game)
            SoundEffectService.shared.play(.click)
            logs.append(LogEntry(level: .warning, message: "Moved game to Trash: \(game.name)"))
            refreshLibrary()
        } catch {
            SoundEffectService.shared.play(.error)
            logs.append(LogEntry(level: .error, message: "Failed to move game to Trash: \(error.localizedDescription)"))
        }
    }

    public func deletePortedGame(_ game: PortedGame) { deleteGameToTrash(game) }
    
    public func scanGameFolder(_ folder: URL) {
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

    // MARK: - Library Directional Navigation (Two-Row Shelves)
    public func handleLibraryNavigateLeft() {
        guard !isGameActionMenuPresented && focusedGameIndex > 0 else { return }
        focusedGameIndex -= 1
        SoundEffectService.shared.play(.focus)
    }

    public func handleLibraryNavigateRight() {
        guard !isGameActionMenuPresented && focusedGameIndex < libraryGames.count - 1 else { return }
        focusedGameIndex += 1
        SoundEffectService.shared.play(.focus)
    }

    public func handleLibraryNavigateUp() {
        if isGameActionMenuPresented {
            if gameActionFocusedIndex > 0 {
                gameActionFocusedIndex -= 1
                SoundEffectService.shared.play(.focus)
            }
            return
        }
        let macs = macLibraryGames
        let ioses = iosLibraryGames
        if let cur = currentFocusedGame, let iosIdx = ioses.firstIndex(where: { $0.id == cur.id }), !macs.isEmpty {
            let targetMacIdx = min(iosIdx, macs.count - 1)
            if let newGameIdx = libraryGames.firstIndex(where: { $0.id == macs[targetMacIdx].id }) {
                focusedGameIndex = newGameIdx
                SoundEffectService.shared.play(.focus)
            }
        }
    }

    public func handleLibraryNavigateDown() {
        if isGameActionMenuPresented {
            if gameActionFocusedIndex < 6 {
                gameActionFocusedIndex += 1
                SoundEffectService.shared.play(.focus)
            }
            return
        }
        let macs = macLibraryGames
        let ioses = iosLibraryGames
        if let cur = currentFocusedGame, let macIdx = macs.firstIndex(where: { $0.id == cur.id }), !ioses.isEmpty {
            let targetIosIdx = min(macIdx, ioses.count - 1)
            if let newGameIdx = libraryGames.firstIndex(where: { $0.id == ioses[targetIosIdx].id }) {
                focusedGameIndex = newGameIdx
                SoundEffectService.shared.play(.focus)
            }
        }
    }
}
