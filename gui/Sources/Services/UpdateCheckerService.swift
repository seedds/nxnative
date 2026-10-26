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
            
            self.delegate?.updateCheckerDidProduceLog("==> Recovering any interrupted git operations...")
            // Abort any previously stalled rebase or merge
            _ = self.runGitWithStatus(["rebase", "--abort"], in: nxnativePath)
            _ = self.runGitWithStatus(["merge", "--abort"], in: nxnativePath)
            
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
            
            self.delegate?.updateCheckerDidProduceLog("==> Fetching latest changes from upstream GitHub...")
            let fetchRes = self.runGitWithStatus(["fetch", "origin", "main"], in: nxnativePath)
            self.delegate?.updateCheckerDidProduceLog(fetchRes.output)
            
            self.delegate?.updateCheckerDidProduceLog("==> Merging upstream updates into local branch...")
            let mergeRes = self.runGitWithStatus(["merge", "-X", "ours", "--no-edit", "origin/main"], in: nxnativePath)
            self.delegate?.updateCheckerDidProduceLog(mergeRes.output)
            
            let isSuccess = (mergeRes.exitCode == 0) &&
                !mergeRes.output.localizedCaseInsensitiveContains("fatal:") &&
                !mergeRes.output.localizedCaseInsensitiveContains("Automatic merge failed")
            
            if !isSuccess {
                _ = self.runGitWithStatus(["merge", "--abort"], in: nxnativePath)
            }
            
            if didStash {
                self.delegate?.updateCheckerDidProduceLog("==> Restoring local modifications...")
                _ = self.runGitWithStatus(["stash", "pop"], in: nxnativePath)
            }
            
            // Update submodules if any
            _ = self.runGitWithStatus(["submodule", "update", "--init", "--recursive"], in: nxnativePath)
            
            let newHead = self.runGit(["rev-parse", "--short", "HEAD"], in: nxnativePath).trimmingCharacters(in: .whitespacesAndNewlines)
            self.isUpdating = false
            
            DispatchQueue.main.async {
                if isSuccess {
                    completion(true, "Successfully updated repository to commit [\(newHead)]!")
                } else {
                    let errOut = mergeRes.output.trimmingCharacters(in: .whitespacesAndNewlines)
                    completion(false, "Update failed: \(errOut.isEmpty ? "Merge conflict or network error" : errOut)")
                }
            }
        }
    }
    
    @discardableResult
    private func runGitWithStatus(_ args: [String], in dir: String) -> (exitCode: Int32, output: String) {
        let fm = FileManager.default
        let candidates = ["/opt/homebrew/bin/git", "/usr/local/bin/git", "/usr/bin/git"]
        let gitPath = candidates.first(where: { fm.fileExists(atPath: $0) }) ?? "/usr/bin/git"
        
        let process = Process()
        process.executableURL = URL(fileURLWithPath: gitPath)
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
