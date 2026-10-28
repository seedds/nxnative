import Foundation

public protocol InstallerServiceDelegate: AnyObject {
    func installerDidProduceOutput(_ line: String)
    func installerDidStartItem(_ name: String)
    func installerDidFinishItem(_ name: String, success: Bool)
    func installerDidCompleteAll(allSuccess: Bool)
}

public final class InstallerService {
    public static let shared = InstallerService()
    
    public weak var delegate: InstallerServiceDelegate?
    private var isRunning = false
    private var currentProcess: Process?
    
    private init() {}
    
    public func install(dependency: DependencyStatus, completion: @escaping (Bool) -> Void) {
        guard let cmd = dependency.installCommand, !cmd.isEmpty, !isRunning else {
            completion(false)
            return
        }
        executeAction(title: "Installing: \(dependency.name)", command: cmd, completion: completion)
    }
    
    public func repair(dependency: DependencyStatus, completion: @escaping (Bool) -> Void) {
        guard let cmd = dependency.repairCommand ?? dependency.installCommand, !cmd.isEmpty, !isRunning else {
            completion(false)
            return
        }
        executeAction(title: "Repairing: \(dependency.name)", command: cmd, completion: completion)
    }
    
    public func uninstall(dependency: DependencyStatus, completion: @escaping (Bool) -> Void) {
        guard !dependency.isProtected else {
            delegate?.installerDidProduceOutput("[WARN] \(dependency.name) is an essential system tool and cannot be uninstalled.")
            completion(false)
            return
        }
        guard let cmd = dependency.uninstallCommand, !cmd.isEmpty, !isRunning else {
            completion(false)
            return
        }
        executeAction(title: "Uninstalling: \(dependency.name)", command: cmd, completion: completion)
    }
    
    private func executeAction(title: String, command: String, completion: @escaping (Bool) -> Void) {
        isRunning = true
        delegate?.installerDidStartItem(title)
        delegate?.installerDidProduceOutput("==> \(title)")
        delegate?.installerDidProduceOutput("Command: \(command)")
        
        runShell(command: command) { [weak self] success, output in
            self?.isRunning = false
            self?.delegate?.installerDidFinishItem(title, success: success)
            completion(success)
        }
    }
    
    public func installAll(dependencies: [DependencyStatus], completion: @escaping (Bool) -> Void) {
        guard !dependencies.isEmpty, !isRunning else {
            completion(true)
            return
        }
        
        isRunning = true
        var queue = dependencies.filter { $0.installCommand != nil }
        var overallSuccess = true
        
        func processNext() {
            guard !queue.isEmpty else {
                self.isRunning = false
                self.delegate?.installerDidCompleteAll(allSuccess: overallSuccess)
                completion(overallSuccess)
                return
            }
            
            let item = queue.removeFirst()
            let currentStep = dependencies.count - queue.count
            let totalSteps = dependencies.count
            self.delegate?.installerDidStartItem("\(item.name) (\(currentStep)/\(totalSteps))")
            self.delegate?.installerDidProduceOutput("\n----------------------------------------")
            self.delegate?.installerDidProduceOutput("==> Installing (\(currentStep)/\(totalSteps)): \(item.name)")
            self.delegate?.installerDidProduceOutput("Command: \(item.installCommand!)")
            
            self.runShell(command: item.installCommand!) { [weak self] success, output in
                guard let self = self else { return }
                if !success {
                    overallSuccess = false
                    self.delegate?.installerDidProduceOutput("[FAIL] Failed to install \(item.name).")
                } else {
                    self.delegate?.installerDidProduceOutput("[OK] \(item.name) installed successfully.")
                }
                self.delegate?.installerDidFinishItem(item.name, success: success)
                processNext()
            }
        }
        
        processNext()
    }
    
    public func repairAll(dependencies: [DependencyStatus], completion: @escaping (Bool) -> Void) {
        guard !dependencies.isEmpty, !isRunning else {
            completion(true)
            return
        }
        
        isRunning = true
        var queue = dependencies.filter { $0.repairCommand != nil }
        var overallSuccess = true
        
        func processNext() {
            guard !queue.isEmpty else {
                self.isRunning = false
                self.delegate?.installerDidCompleteAll(allSuccess: overallSuccess)
                completion(overallSuccess)
                return
            }
            
            let item = queue.removeFirst()
            self.delegate?.installerDidStartItem(item.name)
            self.delegate?.installerDidProduceOutput("\n----------------------------------------")
            self.delegate?.installerDidProduceOutput("==> Repairing: \(item.name)")
            self.delegate?.installerDidProduceOutput("Command: \(item.repairCommand!)")
            
            self.runShell(command: item.repairCommand!) { [weak self] success, output in
                guard let self = self else { return }
                if !success {
                    overallSuccess = false
                    self.delegate?.installerDidProduceOutput("[FAIL] Failed to repair \(item.name).")
                } else {
                    self.delegate?.installerDidProduceOutput("[OK] \(item.name) repaired successfully.")
                }
                self.delegate?.installerDidFinishItem(item.name, success: success)
                processNext()
            }
        }
        
        processNext()
    }
    
    private func runShell(command: String, completion: @escaping (Bool, String) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/bin/zsh")
            process.arguments = ["-l", "-c", command]
            
            var env = ProcessInfo.processInfo.environment
            let extraPaths = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
            if let existing = env["PATH"] {
                env["PATH"] = "\(extraPaths):\(existing)"
            } else {
                env["PATH"] = extraPaths
            }
            
            // Configure SUDO_ASKPASS for unattended / GUI password prompts if required
            let askPassURL = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("nxnative_askpass.sh")
            let askPassScript = """
            #!/bin/bash
            exec osascript -e 'display dialog "NxNative Launcher requires administrator privileges to install dependencies:" default answer "" with hidden answer' -e 'text returned of result'
            """
            if !FileManager.default.fileExists(atPath: askPassURL.path) {
                try? askPassScript.write(to: askPassURL, atomically: true, encoding: .utf8)
                chmod(askPassURL.path, 0o755)
            }
            env["SUDO_ASKPASS"] = askPassURL.path
            process.environment = env
            
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            
            self?.currentProcess = process
            
            let handle = pipe.fileHandleForReading
            var fullOutput = ""
            
            handle.readabilityHandler = { [weak self] h in
                let data = h.availableData
                guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
                fullOutput += text
                DispatchQueue.main.async {
                    self?.delegate?.installerDidProduceOutput(text.trimmingCharacters(in: .newlines))
                }
            }
            
            do {
                try process.run()
                process.waitUntilExit()
                handle.readabilityHandler = nil
                
                var success = process.terminationStatus == 0
                if command.contains("xcode-select") && (fullOutput.contains("install requested") || fullOutput.contains("already installed")) {
                    success = true
                    DispatchQueue.main.async {
                        self?.delegate?.installerDidProduceOutput("Apple installation dialog prompted on screen. Complete Apple's installer then click Re-check.")
                    }
                }
                DispatchQueue.main.async {
                    completion(success, fullOutput)
                }
            } catch {
                handle.readabilityHandler = nil
                DispatchQueue.main.async {
                    self?.delegate?.installerDidProduceOutput("Error running command: \(error.localizedDescription)")
                    completion(false, error.localizedDescription)
                }
            }
        }
    }
}
