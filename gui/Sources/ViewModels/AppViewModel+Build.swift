import Foundation
import AppKit

extension AppViewModel {
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
    
    // MARK: - BuildRunner Delegates
    public func buildRunner(_ runner: BuildRunner, didProduceLog entry: LogEntry) {
        logs.append(entry)
    }
    
    public func buildRunner(_ runner: BuildRunner, didUpdateStage stage: String, index: Int, total: Int) {
        self.currentStageName = stage
        self.stageIndex = index
        self.totalStages = total
        
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
        
        if let out = outputPath {
            let targetURL = URL(fileURLWithPath: out)
            GameCoverService.shared.injectCoverIntoBundle(at: targetURL, gameName: appName.isEmpty ? (selectedGame?.name ?? "Game") : appName, titleId: selectedGame?.titleId)
        }
        
        refreshLibrary()
        
        SoundEffectService.shared.play(.launch)
        NotificationService.shared.sendPortSuccessNotification(gameName: appName.isEmpty ? "Ported Game" : appName, outputPath: outputPath)
    }
    
    public func buildRunner(_ runner: BuildRunner, didFinishWithError error: String) {
        self.isBuilding = false
        if let idx = buildStages.firstIndex(where: { $0.isRunning }) {
            buildStages[idx].isRunning = false
            buildStages[idx].isFailed = true
        }
        self.buildError = error
        self.logs.append(LogEntry(level: .error, message: error))
        
        SoundEffectService.shared.play(.error)
        NotificationService.shared.sendPortErrorNotification(gameName: appName.isEmpty ? "Game" : appName, error: error)
    }
}
