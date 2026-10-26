import Foundation

public struct UpdateInfo {
    public let hasUpdate: Bool
    public let localCommit: String
    public let remoteCommit: String
    public let commitMessages: [String]
    public let checkDate: Date
}

public protocol UpdateCheckerDelegate: AnyObject {
    func updateCheckerDidFindUpdate(_ info: UpdateInfo)
    func updateCheckerDidProduceLog(_ message: String)
}

public final class UpdateCheckerService {
    public static let shared = UpdateCheckerService()
    
    public weak var delegate: UpdateCheckerDelegate?
    private var isChecking = false
    private var isUpdating = false
    
    private init() {}
    
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
                checkDate: Date()
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
    
    public func applyUpdate(nxnativePath: String, completion: @escaping (Bool, String) -> Void) {
        guard !isUpdating else {
            completion(false, "Update already in progress")
            return
        }
        isUpdating = true
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            self.delegate?.updateCheckerDidProduceLog("==> Checking repository status before update...")
            
            // Check for uncommitted changes
            let statusRes = self.runGitWithStatus(["status", "--porcelain"], in: nxnativePath)
            let hasUncommitted = !statusRes.output.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            
            var didStash = false
            if hasUncommitted {
                self.delegate?.updateCheckerDidProduceLog("==> Stashing local modifications temporarily...")
                let stashRes = self.runGitWithStatus(["stash", "push", "-u", "-m", "nxnative_update_auto_stash"], in: nxnativePath)
                didStash = (stashRes.exitCode == 0)
            }
            
            self.delegate?.updateCheckerDidProduceLog("==> Pulling latest updates from GitHub (git pull --rebase origin main)...")
            let pullResult = self.runGitWithStatus(["pull", "--rebase", "origin", "main"], in: nxnativePath)
            self.delegate?.updateCheckerDidProduceLog(pullResult.output)
            
            if didStash {
                self.delegate?.updateCheckerDidProduceLog("==> Restoring local modifications...")
                _ = self.runGitWithStatus(["stash", "pop"], in: nxnativePath)
            }
            
            // Update submodules if any
            _ = self.runGitWithStatus(["submodule", "update", "--init", "--recursive"], in: nxnativePath)
            
            let newHead = self.runGit(["rev-parse", "--short", "HEAD"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
            self.isUpdating = false
            
            let isSuccess = (pullResult.exitCode == 0) &&
                !pullResult.output.localizedCaseInsensitiveContains("error:") &&
                !pullResult.output.localizedCaseInsensitiveContains("fatal:") &&
                !pullResult.output.localizedCaseInsensitiveContains("Aborting")
            
            DispatchQueue.main.async {
                if isSuccess {
                    completion(true, "Successfully updated to commit [\(newHead)]!")
                } else {
                    completion(false, "Update failed: \(pullResult.output.trimmingCharacters(in: .whitespacesAndNewlines))")
                }
            }
        }
    }
    
    @discardableResult
    private func runGitWithStatus(_ args: [String], in dir: String) -> (exitCode: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/git")
        process.arguments = args
        process.currentDirectoryURL = URL(fileURLWithPath: dir)
        
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"
        process.environment = env
        
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        
        do {
            try process.run()
            process.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            let out = String(data: data, encoding: .utf8) ?? ""
            return (process.terminationStatus, out)
        } catch {
            return (-1, error.localizedDescription)
        }
    }
    
    private func runGit(_ args: [String], in dir: String) -> String {
        return runGitWithStatus(args, in: dir).output
    }
}
