import Foundation
import AppKit

public final class UpdateCheckerService {
    public static let shared = UpdateCheckerService()
    public static let launcherRepoURL = "https://github.com/rennan101/nxnative_Launcher.git"
    
    public weak var delegate: UpdateCheckerDelegate?
    private var isChecking = false
    private var isUpdating = false
    
    private init() {}
    
    public func getLocalCommit(nxnativePath: String) -> String {
        guard !nxnativePath.isEmpty else { return "HEAD" }
        let localHead = runGit(["rev-parse", "--short", "HEAD"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
        return localHead.isEmpty ? "HEAD" : localHead
    }
    
    public func checkForUpdates(nxnativePath: String, completion: @escaping (UpdateInfo?) -> Void) {
        guard !isChecking else { return }
        isChecking = true
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            let fm = FileManager.default
            let gitDir = URL(fileURLWithPath: nxnativePath).appendingPathComponent(".git").path
            guard fm.fileExists(atPath: gitDir) else {
                self.isChecking = false
                DispatchQueue.main.async { completion(nil) }
                return
            }
            
            // Determine remote to check (prefer origin pointing to rennan101/nxnative_Launcher)
            let remoteURL = self.runGit(["remote", "get-url", "origin"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
            
            // 1. git fetch origin main
            let fetchRes = self.runGitWithStatus(["fetch", "origin", "main", "--quiet"], in: nxnativePath)
            if fetchRes.exitCode != 0 {
                // If fetch fails (no network, etc.), do not report spurious updates
                self.isChecking = false
                DispatchQueue.main.async { completion(nil) }
                return
            }
            
            // 2. Local & Remote HEAD commits
            let localHead = self.runGit(["rev-parse", "--short", "HEAD"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
            let remoteHead = self.runGit(["rev-parse", "--short", "origin/main"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
            
            // 3. Count commits that origin/main has ahead of HEAD
            let countStr = self.runGit(["rev-list", "--count", "HEAD..origin/main"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
            let aheadCount = Int(countStr) ?? 0
            
            // 4. Log of incoming commits
            let logOutput = self.runGit(["log", "HEAD..origin/main", "--oneline"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
            let commits = logOutput.isEmpty ? [] : logOutput.components(separatedBy: .newlines).filter { !$0.isEmpty }
            
            // Only consider it an update if origin/main actually has commits ahead
            let hasUpdate = aheadCount > 0 && !commits.isEmpty && localHead != remoteHead
            let info = UpdateInfo(
                hasUpdate: hasUpdate,
                localCommit: localHead,
                remoteCommit: remoteHead,
                commitMessages: commits,
                checkDate: Date(),
                repoURL: remoteURL.isEmpty ? Self.launcherRepoURL : remoteURL
            )
            
            self.isChecking = false
            DispatchQueue.main.async {
                if hasUpdate {
                    self.delegate?.updateCheckerDidFindUpdate(info)
                }
                completion(info)
            }
        }
    }
    
    /// Pulls latest changes from GitHub, applies engine patches, and rebuilds + installs NxNative_Launcher.app automatically
    public func applyUpdate(
        nxnativePath: String,
        rebuildLauncher: Bool = true,
        progressHandler: ((String) -> Void)? = nil,
        completion: @escaping (Bool, String) -> Void
    ) {
        guard !isUpdating else {
            completion(false, "Update already in progress")
            return
        }
        isUpdating = true
        
        let reportProgress: (String) -> Void = { [weak self] msg in
            DispatchQueue.main.async {
                self?.delegate?.updateCheckerDidProduceLog(msg)
                progressHandler?(msg)
            }
        }
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            reportProgress("==> Preparing repository for update...")
            // Abort any previously stalled rebase or merge
            _ = self.runGitWithStatus(["rebase", "--abort"], in: nxnativePath)
            _ = self.runGitWithStatus(["merge", "--abort"], in: nxnativePath)
            
            // Check for uncommitted changes
            let statusRes = self.runGitWithStatus(["status", "--porcelain"], in: nxnativePath)
            let hasUncommitted = !statusRes.output.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            
            var didStash = false
            if hasUncommitted {
                reportProgress("==> Stashing local modifications temporarily...")
                let stashRes = self.runGitWithStatus(["stash", "push", "-u", "-m", "nxnative_update_auto_stash"], in: nxnativePath)
                didStash = (stashRes.exitCode == 0)
            }
            
            reportProgress("==> Downloading latest version from GitHub (rennan101/nxnative_Launcher)...")
            let fetchRes = self.runGitWithStatus(["fetch", "origin", "main"], in: nxnativePath)
            if !fetchRes.output.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                reportProgress(fetchRes.output.trimmingCharacters(in: .whitespacesAndNewlines))
            }
            
            reportProgress("==> Merging GitHub updates into local branch...")
            let mergeRes = self.runGitWithStatus(["merge", "-X", "theirs", "--no-edit", "origin/main"], in: nxnativePath)
            if !mergeRes.output.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                reportProgress(mergeRes.output.trimmingCharacters(in: .whitespacesAndNewlines))
            }
            
            let isMergeSuccess = (mergeRes.exitCode == 0) &&
                !mergeRes.output.localizedCaseInsensitiveContains("fatal:") &&
                !mergeRes.output.localizedCaseInsensitiveContains("Automatic merge failed")
            
            if !isMergeSuccess {
                _ = self.runGitWithStatus(["merge", "--abort"], in: nxnativePath)
            }
            
            if didStash {
                reportProgress("==> Restoring local workspace modifications...")
                _ = self.runGitWithStatus(["stash", "pop"], in: nxnativePath)
            }
            
            // Update submodules if any
            _ = self.runGitWithStatus(["submodule", "update", "--init", "--recursive"], in: nxnativePath)
            
            guard isMergeSuccess else {
                self.isUpdating = false
                let errOut = mergeRes.output.trimmingCharacters(in: .whitespacesAndNewlines)
                DispatchQueue.main.async {
                    completion(false, "Git update failed: \(errOut.isEmpty ? "Merge conflict or network error" : errOut)")
                }
                return
            }
            
            // Re-apply compatibility patches
            reportProgress("==> Verifying & applying engine compatibility patches...")
            _ = PatchService.shared.applyPatchesIfNeeded(nxnativePath: nxnativePath)
            
            // Rebuild & install NxNative_Launcher.app if requested
            if rebuildLauncher {
                let buildScript = URL(fileURLWithPath: nxnativePath).appendingPathComponent("scripts/build_gui.sh").path
                if FileManager.default.fileExists(atPath: buildScript) {
                    reportProgress("==> Compiling & installing new NxNative Launcher (.app)...")
                    let buildRes = self.runShellScript(buildScript, workingDir: nxnativePath) { line in
                        reportProgress(line)
                    }
                    if buildRes != 0 {
                        self.isUpdating = false
                        DispatchQueue.main.async {
                            completion(false, "Git updated, but compiling NxNative Launcher failed (exit code \(buildRes)).")
                        }
                        return
                    }
                    
                    // If running from a custom location (e.g. /Applications/NxNative_Launcher.app), sync binary there too
                    self.syncUpdatedBundleToRunningAppIfNeeded(nxnativePath: nxnativePath, reportProgress: reportProgress)
                }
            }
            
            let newHead = self.runGit(["rev-parse", "--short", "HEAD"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
            self.isUpdating = false
            
            DispatchQueue.main.async {
                completion(true, "NxNative Launcher updated & installed successfully (commit [\(newHead)])!")
            }
        }
    }
    
    /// Relaunches the updated NxNative_Launcher.app bundle and terminates the current process
    public func relaunchLauncher(nxnativePath: String) {
        let fm = FileManager.default
        let workspaceRoot = URL(fileURLWithPath: nxnativePath).deletingLastPathComponent()
        let workspaceBundle = workspaceRoot.appendingPathComponent("NxNative_Launcher.app").path
        let repoBundle = URL(fileURLWithPath: nxnativePath).appendingPathComponent("NxNative_Launcher.app").path
        let currentBundle = Bundle.main.bundleURL.path
        
        let targetBundle: String
        if currentBundle.hasSuffix(".app") && fm.fileExists(atPath: currentBundle) {
            targetBundle = currentBundle
        } else if fm.fileExists(atPath: workspaceBundle) {
            targetBundle = workspaceBundle
        } else {
            targetBundle = repoBundle
        }
        
        let pid = ProcessInfo.processInfo.processIdentifier
        let script = "while kill -0 \(pid) 2>/dev/null; do sleep 0.2; done; sleep 0.3; open \"\(targetBundle)\""
        
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", script]
        try? process.run()
        
        DispatchQueue.main.async {
            NSApplication.shared.terminate(nil)
        }
    }
    
    private func syncUpdatedBundleToRunningAppIfNeeded(nxnativePath: String, reportProgress: (String) -> Void) {
        let fm = FileManager.default
        let currentBundleURL = Bundle.main.bundleURL.standardizedFileURL
        guard currentBundleURL.pathExtension == "app" else { return }
        
        let workspaceRoot = URL(fileURLWithPath: nxnativePath).deletingLastPathComponent().standardizedFileURL
        let workspaceApp = workspaceRoot.appendingPathComponent("NxNative_Launcher.app").standardizedFileURL
        let repoApp = URL(fileURLWithPath: nxnativePath).appendingPathComponent("NxNative_Launcher.app").standardizedFileURL
        
        if currentBundleURL.path != workspaceApp.path && currentBundleURL.path != repoApp.path {
            let sourceApp = fm.fileExists(atPath: workspaceApp.path) ? workspaceApp : repoApp
            guard fm.fileExists(atPath: sourceApp.path) else { return }
            reportProgress("==> Updating running bundle at \(currentBundleURL.path)...")
            let cpProc = Process()
            cpProc.executableURL = URL(fileURLWithPath: "/bin/cp")
            cpProc.arguments = ["-Rf", "\(sourceApp.path)/Contents", "\(currentBundleURL.path)/"]
            try? cpProc.run()
            cpProc.waitUntilExit()
        }
    }
    
    private func runShellScript(_ scriptPath: String, workingDir: String, onLine: @escaping (String) -> Void) -> Int32 {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        process.arguments = [scriptPath]
        process.currentDirectoryURL = URL(fileURLWithPath: workingDir)
        process.environment = ["PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"]
        
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        let handle = pipe.fileHandleForReading
        handle.readabilityHandler = { fileHandle in
            let data = fileHandle.availableData
            guard !data.isEmpty, let chunk = String(data: data, encoding: .utf8) else { return }
            chunk.components(separatedBy: .newlines)
                .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }
                .forEach { onLine($0) }
        }
        
        do {
            try process.run()
            process.waitUntilExit()
            handle.readabilityHandler = nil
            return process.terminationStatus
        } catch {
            handle.readabilityHandler = nil
            onLine("Error running build script: \(error.localizedDescription)")
            return -1
        }
    }
    
    @discardableResult
    private func runGitWithStatus(_ args: [String], in dir: String) -> (exitCode: Int32, output: String) {
        let fm = FileManager.default
        let gitPath = ["/opt/homebrew/bin/git", "/usr/local/bin/git", "/usr/bin/git"].first(where: { fm.fileExists(atPath: $0) }) ?? "/usr/bin/git"
        let process = Process()
        process.executableURL = URL(fileURLWithPath: gitPath)
        process.arguments = args
        process.currentDirectoryURL = URL(fileURLWithPath: dir)
        process.environment = ["PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        
        do {
            try process.run()
            process.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            return (process.terminationStatus, String(data: data, encoding: .utf8) ?? "")
        } catch {
            return (-1, error.localizedDescription)
        }
    }
    
    private func runGit(_ args: [String], in dir: String) -> String {
        runGitWithStatus(args, in: dir).output
    }
}
