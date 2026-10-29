import Foundation

public protocol BuildRunnerDelegate: AnyObject {
    func buildRunner(_ runner: BuildRunner, didProduceLog entry: LogEntry)
    func buildRunner(_ runner: BuildRunner, didUpdateStage stage: String, index: Int, total: Int)
    func buildRunner(_ runner: BuildRunner, didUpdateStep step: String)
    func buildRunner(_ runner: BuildRunner, didUpdateProgress fraction: Double, doneBytes: Double?, totalBytes: Double?, etaSeconds: Double?)
    func buildRunner(_ runner: BuildRunner, didEndStage stage: String, index: Int, ok: Bool, elapsed: Double?, summary: String?)
    func buildRunner(_ runner: BuildRunner, didFinishSuccess outputPath: String?)
    func buildRunner(_ runner: BuildRunner, didFinishWithError error: String)
}

public final class BuildRunner {
    private var process: Process?
    private var stdoutPipe: Pipe?
    private var stderrPipe: Pipe?
    private var isCancelled = false
    
    public weak var delegate: BuildRunnerDelegate?
    
    public init() {}
    
    public var isRunning: Bool {
        process?.isRunning ?? false
    }
    
    public func startBuild(
        nxnativePath: String,
        gameDirectory: URL,
        prodKeysPath: String,
        platform: TargetPlatform,
        customOutputFolder: URL?,
        appName: String?,
        bundleId: String?,
        language: String?,
        showFps: Bool,
        keepExtracted: Bool,
        surveyOnly: Bool,
        romfsSeparate: Bool = false
    ) {
        guard !isRunning else { return }
        isCancelled = false
        
        let scriptPath = URL(fileURLWithPath: nxnativePath)
            .appendingPathComponent("scripts/build_ipa_from_nsp.py")
            .standardized.path
        
        guard FileManager.default.fileExists(atPath: scriptPath) else {
            delegate?.buildRunner(self, didFinishWithError: "Build script not found at: \(scriptPath)")
            return
        }
        
        // Ensure engine patches (60 FPS, allocators, shader fallback) are applied
        PatchService.shared.applyPatchesIfNeeded(nxnativePath: nxnativePath) { [weak self] message, isSuccess in
            guard let self = self else { return }
            self.delegate?.buildRunner(self, didProduceLog: LogEntry(level: isSuccess ? .success : .info, message: message))
        }
        
        // Determina caminho do python com seleção resiliente
        let pyCandidates = [
            "/opt/homebrew/opt/python@3.14/bin/python3",
            "/opt/homebrew/opt/python@3.13/bin/python3",
            "/opt/homebrew/opt/python@3.12/bin/python3",
            "/opt/homebrew/bin/python3",
            "/usr/local/opt/python@3.14/bin/python3",
            "/usr/local/opt/python@3.13/bin/python3",
            "/usr/local/opt/python@3.12/bin/python3",
            "/usr/local/bin/python3",
            "/usr/bin/python3"
        ]
        let fm = FileManager.default
        var pythonPath = "/usr/bin/python3"
        for candidate in pyCandidates where fm.fileExists(atPath: candidate) {
            pythonPath = candidate
            break
        }
        
        var arguments: [String] = [
            scriptPath,
            gameDirectory.path,
            "-k", prodKeysPath,
            "--progress", "json"
        ]
        
        // Plataforma
        switch platform {
        case .ios:
            arguments.append(contentsOf: ["--platform", "ios"])
        case .macApp:
            arguments.append(contentsOf: ["--platform", "mac-app"])
        }
        
        // Output path (always explicitly provided so .app is generated for Mac App)
        let outDir = customOutputFolder ?? gameDirectory
        let rawName = (appName?.isEmpty == false ? appName! : gameDirectory.lastPathComponent)
            .replacingOccurrences(of: "\\[[^\\]]+\\]", with: "", options: .regularExpression)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanName = rawName.isEmpty ? "Game" : rawName
        let ext = (platform == .macApp ? ".app" : ".ipa")
        let outPath = outDir.appendingPathComponent(cleanName + ext).path
        arguments.append(contentsOf: ["-o", outPath])
        
        if let name = appName, !name.trimmingCharacters(in: .whitespaces).isEmpty {
            arguments.append(contentsOf: ["--app-name", name.trimmingCharacters(in: .whitespaces)])
        }
        
        if let bid = bundleId, !bid.trimmingCharacters(in: .whitespaces).isEmpty {
            arguments.append(contentsOf: ["--bundle-id", bid.trimmingCharacters(in: .whitespaces)])
        }
        
        if let lang = language, lang != "auto", !lang.isEmpty {
            arguments.append(contentsOf: ["-l", lang])
        }
        
        if showFps {
            arguments.append("--show-fps")
        } else {
            arguments.append("--no-show-fps")
        }
        
        let shouldSeparateRomfs = (platform == .ios && romfsSeparate)
        if shouldSeparateRomfs {
            arguments.append(contentsOf: ["--romfs", "separate"])
            // --romfs separate requires --keep-extracted
            arguments.append("--keep-extracted")
        } else {
            if keepExtracted {
                arguments.append("--keep-extracted")
            } else {
                arguments.append("--no-keep-extracted")
            }
        }
        
        if surveyOnly {
            arguments.append("--survey-only")
        }
        
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: pythonPath)
        proc.arguments = arguments
        proc.currentDirectoryURL = URL(fileURLWithPath: nxnativePath)
        
        // System PATH to ensure homebrew and local tools are available
        var env = ProcessInfo.processInfo.environment
        let brewPaths = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
        if let existing = env["PATH"] {
            env["PATH"] = "\(brewPaths):\(existing)"
        } else {
            env["PATH"] = brewPaths
        }
        env["PYTHONUNBUFFERED"] = "1"
        proc.environment = env
        
        let stdout = Pipe()
        let stderr = Pipe()
        proc.standardOutput = stdout
        proc.standardError = stderr
        
        self.process = proc
        self.stdoutPipe = stdout
        self.stderrPipe = stderr
        
        delegate?.buildRunner(self, didProduceLog: LogEntry(level: .info, message: "Starting: \(pythonPath) \(arguments.joined(separator: " "))"))
        
        // Read stdout
        let stdoutHandle = stdout.fileHandleForReading
        stdoutHandle.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let self = self else { return }
            self.processOutputData(data, isError: false)
        }
        
        // Leitura de stderr
        let stderrHandle = stderr.fileHandleForReading
        stderrHandle.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let self = self else { return }
            self.processOutputData(data, isError: true)
        }
        
        proc.terminationHandler = { [weak self] p in
            guard let self = self else { return }
            stdoutHandle.readabilityHandler = nil
            stderrHandle.readabilityHandler = nil
            
            DispatchQueue.main.async {
                if self.isCancelled {
                    self.delegate?.buildRunner(self, didFinishWithError: "Build cancelled by user.")
                } else if p.terminationStatus == 0 {
                    let outPath = self.resolveFinalOutputPath(gameDir: gameDirectory, customOut: customOutputFolder, platform: platform, appName: appName)
                    self.delegate?.buildRunner(self, didFinishSuccess: outPath)
                } else {
                    self.delegate?.buildRunner(self, didFinishWithError: "Process exited with error (code: \(p.terminationStatus)). Check logs for details.")
                }
            }
        }
        
        do {
            try proc.run()
        } catch {
            delegate?.buildRunner(self, didFinishWithError: "Failed to start build process: \(error.localizedDescription)")
        }
    }
    
    public func cancel() {
        guard isRunning else { return }
        isCancelled = true
        delegate?.buildRunner(self, didProduceLog: LogEntry(level: .warning, message: "Cancelling build process..."))
        process?.terminate()
    }
    
    private var stdoutBuffer = ""
    private var stderrBuffer = ""
    private let decoder = JSONDecoder()
    
    private func processOutputData(_ data: Data, isError: Bool) {
        guard let text = String(data: data, encoding: .utf8) else { return }
        
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            if isError {
                self.stderrBuffer += text
                while let newlineRange = self.stderrBuffer.range(of: "\n") {
                    let line = String(self.stderrBuffer[..<newlineRange.lowerBound]).trimmingCharacters(in: .whitespacesAndNewlines)
                    self.stderrBuffer.removeSubrange(..<newlineRange.upperBound)
                    if !line.isEmpty {
                        self.handleOutputLine(line, isError: true)
                    }
                }
            } else {
                self.stdoutBuffer += text
                while let newlineRange = self.stdoutBuffer.range(of: "\n") {
                    let line = String(self.stdoutBuffer[..<newlineRange.lowerBound]).trimmingCharacters(in: .whitespacesAndNewlines)
                    self.stdoutBuffer.removeSubrange(..<newlineRange.upperBound)
                    if !line.isEmpty {
                        self.handleOutputLine(line, isError: false)
                    }
                }
            }
        }
    }
    
    private func handleOutputLine(_ line: String, isError: Bool) {
        if line.hasPrefix("{") && line.hasSuffix("}"),
           let jsonData = line.data(using: .utf8),
           let event = try? decoder.decode(ProgressEvent.self, from: jsonData) {
            
            switch event.ev {
            case "stage":
                let stageName = event.name ?? "Processing"
                let k = event.k ?? 1
                let n = event.n ?? 1
                delegate?.buildRunner(self, didUpdateStage: stageName, index: k, total: n)
                delegate?.buildRunner(self, didProduceLog: LogEntry(level: .step, message: "[\(k)/\(n)] \(stageName)"))
                
            case "step":
                if let step = event.step {
                    delegate?.buildRunner(self, didUpdateStep: step)
                    delegate?.buildRunner(self, didProduceLog: LogEntry(level: .info, message: "↳ \(step)"))
                }
                
            case "progress":
                let frac = event.frac ?? 0.0
                delegate?.buildRunner(self, didUpdateProgress: frac, doneBytes: event.done, totalBytes: event.total, etaSeconds: event.eta)
                
            case "stage_end":
                let ok = event.ok ?? true
                let name = event.name ?? ""
                let summary = event.summary ?? ""
                let k = event.k ?? 1
                delegate?.buildRunner(self, didEndStage: name, index: k, ok: ok, elapsed: event.elapsed, summary: summary)
                let msg = ok ? "[OK] \(name) completed \(summary)" : "[FAIL] \(name) failed \(summary)"
                delegate?.buildRunner(self, didProduceLog: LogEntry(level: ok ? .success : .error, message: msg))
                
            case "say":
                if let msg = event.msg {
                    delegate?.buildRunner(self, didProduceLog: LogEntry(level: .info, message: msg))
                }
                
            default:
                break
            }
        } else {
            let level: LogLevel = isError ? .error : .info
            delegate?.buildRunner(self, didProduceLog: LogEntry(level: level, message: line))
        }
    }
    
    private func resolveFinalOutputPath(gameDir: URL, customOut: URL?, platform: TargetPlatform, appName: String?) -> String {
        let searchDir = customOut ?? gameDir
        let fm = FileManager.default
        let ext = (platform == .macApp) ? "app" : "ipa"
        
        // Search for newly generated file in destination folder
        if let items = try? fm.contentsOfDirectory(at: searchDir, includingPropertiesForKeys: [.contentModificationDateKey]) {
            let matching = items.filter { $0.pathExtension.lowercased() == ext }
                .sorted {
                    let date1 = (try? $0.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate ?? Date.distantPast
                    let date2 = (try? $1.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate ?? Date.distantPast
                    return date1 > date2
                }
            if let first = matching.first {
                return first.path
            }
        }
        
        return searchDir.path
    }
}
