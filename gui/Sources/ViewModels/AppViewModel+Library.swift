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
        guard launchingGameId == nil else { return }
        launchingGameId = game.id
        SoundEffectService.shared.play(.launch)
        
        let config = GameRuntimeConfig.load(for: game)
        config.applyToDisk(for: game)
        InputProfileManager.shared.exportActiveMappingToDisk(libraryFolder: libraryFolder)
        
        LibraryScanner.shared.launchGame(at: game.url, env: config.toEnvironmentDict()) { [weak self] success, error in
            DispatchQueue.main.async {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.8) {
                    self?.launchingGameId = nil
                }
                if success {
                    self?.logs.append(LogEntry(level: .success, message: "Launched game: \(game.name)"))
                } else if let err = error {
                    SoundEffectService.shared.play(.error)
                    self?.logs.append(LogEntry(level: .error, message: "Failed to launch game: \(err)"))
                }
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
    
    public func revealSaveFolder(_ url: URL) {
        let fm = FileManager.default
        if !fm.fileExists(atPath: url.path) {
            try? fm.createDirectory(at: url, withIntermediateDirectories: true)
        }
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
}
