import Foundation

public struct ProdKeysDetectionResult {
    public let path: String
    public let sourceName: String
    public let isValid: Bool
}

public struct NxNativeDetectionResult {
    public let path: String
    public let isValid: Bool
}

public final class SystemDetector {
    public static let shared = SystemDetector()
    
    private init() {}
    
    /// Resolves the true workspace directory (folder containing NXNative.app or external SSD)
    public func resolveRootDirectory() -> URL {
        let bundleDir = Bundle.main.bundleURL.deletingLastPathComponent()
        let fm = FileManager.default
        
        // 1. Check if nxnative is a sibling of the bundle
        if fm.fileExists(atPath: bundleDir.appendingPathComponent("nxnative").path) {
            return bundleDir
        }
        
        // 2. Check the specific external SSD location
        let ssdPath = URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/NativePort")
        if fm.fileExists(atPath: ssdPath.path) {
            return ssdPath
        }
        
        // 3. Fallback to bundle directory, ensuring it is never "/" or system Applications
        if bundleDir.path != "/" && !bundleDir.path.hasPrefix("/Applications") {
            return bundleDir
        }
        
        return URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("NativePort")
    }
    
    /// Locates the nxnative repository directory
    public func detectNxnativeDirectory(customDir: String? = nil) -> NxNativeDetectionResult {
        let root = resolveRootDirectory()
        let fileManager = FileManager.default
        
        var candidatePaths: [String] = []
        if let custom = customDir, !custom.isEmpty {
            candidatePaths.append(custom)
        }
        candidatePaths.append(contentsOf: [
            root.appendingPathComponent("nxnative").standardized.path,
            root.standardized.path,
            Bundle.main.resourceURL?.appendingPathComponent("nxnative").path ?? ""
        ])
        
        for path in candidatePaths where !path.isEmpty {
            let scriptPath = URL(fileURLWithPath: path).appendingPathComponent("scripts/build_ipa_from_nsp.py").path
            let dmPath = URL(fileURLWithPath: path).appendingPathComponent("dm").path
            
            if fileManager.fileExists(atPath: scriptPath) && fileManager.fileExists(atPath: dmPath) {
                return NxNativeDetectionResult(path: path, isValid: true)
            }
        }
        
        let fallbackPath = root.appendingPathComponent("nxnative").path
        return NxNativeDetectionResult(path: fallbackPath, isValid: false)
    }
    
    /// Clones the official nxnative repository if missing
    public func cloneNxnativeRepository(to destinationURL: URL, completion: @escaping (Bool, String) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/git")
            process.arguments = ["clone", "https://github.com/seedds/nxnative.git", destinationURL.path]
            
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            
            do {
                try process.run()
                process.waitUntilExit()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                let output = String(data: data, encoding: .utf8) ?? ""
                let success = process.terminationStatus == 0
                DispatchQueue.main.async {
                    completion(success, output)
                }
            } catch {
                DispatchQueue.main.async {
                    completion(false, error.localizedDescription)
                }
            }
        }
    }
    
    /// Auto-detects prod.keys from Ryujinx and Switch homebrew locations
    public func detectProdKeys(additionalSearchDir: String? = nil) -> ProdKeysDetectionResult? {
        var candidateLocations: [(path: String, name: String)] = [
            (
                NSString(string: "~/Library/Application Support/Ryujinx/system/prod.keys").expandingTildeInPath,
                "Ryujinx (App Support)"
            ),
            (
                NSString(string: "~/Library/Application Support/ryujinx/system/prod.keys").expandingTildeInPath,
                "Ryujinx (App Support)"
            )
        ]
        
        let root = resolveRootDirectory()
        let siblingCandidates = [
            root.deletingLastPathComponent().appendingPathComponent("Riujinx/system/prod.keys").path,
            root.deletingLastPathComponent().appendingPathComponent("Riujinx/ProdKeys.Net-21.2.0/prod.keys").path,
            root.appendingPathComponent("Riujinx/system/prod.keys").path
        ]
        
        for p in siblingCandidates {
            candidateLocations.append((p, "Ryujinx Folder"))
        }
        
        if let customDir = additionalSearchDir, !customDir.isEmpty {
            let direct = URL(fileURLWithPath: customDir).appendingPathComponent("prod.keys").path
            let systemSub = URL(fileURLWithPath: customDir).appendingPathComponent("system/prod.keys").path
            candidateLocations.insert((direct, "Selected Directory"), at: 0)
            candidateLocations.insert((systemSub, "Selected Directory/system"), at: 0)
        }
        
        candidateLocations.append((
            NSString(string: "~/.switch/prod.keys").expandingTildeInPath,
            "Switch Homebrew"
        ))
        
        let fileManager = FileManager.default
        for candidate in candidateLocations {
            var isDir: ObjCBool = false
            if fileManager.fileExists(atPath: candidate.path, isDirectory: &isDir) && !isDir.boolValue {
                if let attrs = try? fileManager.attributesOfItem(atPath: candidate.path),
                   let size = attrs[.size] as? Int64, size > 100 {
                    return ProdKeysDetectionResult(path: candidate.path, sourceName: candidate.name, isValid: true)
                }
            }
        }
        
        return nil
    }
    
    private func runQuickCommand(_ cmd: String, args: [String]) -> (output: String, success: Bool) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: cmd)
        process.arguments = args
        
        var env = ProcessInfo.processInfo.environment
        let extraPaths = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
        if let existing = env["PATH"] {
            env["PATH"] = "\(extraPaths):\(existing)"
        } else {
            env["PATH"] = extraPaths
        }
        process.environment = env
        
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        
        do {
            try process.run()
            process.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            let str = String(data: data, encoding: .utf8) ?? ""
            return (str.trimmingCharacters(in: .whitespacesAndNewlines), process.terminationStatus == 0)
        } catch {
            return (error.localizedDescription, false)
        }
    }
    
    public func checkPrerequisites() -> SystemPrerequisites {
        var prereqs = SystemPrerequisites()
        let fm = FileManager.default
        
        // 1. Xcode & Command Line Tools
        let xcodeRes = runQuickCommand("/usr/bin/xcode-select", args: ["-p"])
        let hasXcodeApp = fm.fileExists(atPath: "/Applications/Xcode.app")
        if xcodeRes.success && !xcodeRes.output.isEmpty {
            let versionRes = runQuickCommand("/usr/bin/xcodebuild", args: ["-version"])
            let firstLine = versionRes.output.components(separatedBy: .newlines).first ?? xcodeRes.output
            prereqs.xcode = DependencyStatus(id: "xcode", name: "Xcode & Command Line Tools", isAvailable: true, details: firstLine, recommendation: nil, installCommand: "xcode-select --install", repairCommand: "xcode-select --reset", uninstallCommand: nil, isProtected: true)
        } else if hasXcodeApp {
            prereqs.xcode = DependencyStatus(id: "xcode", name: "Xcode & Command Line Tools", isAvailable: false, details: "Xcode detected, CLT not linked", recommendation: "Run 'xcode-select --install' or switch to Xcode path", installCommand: "xcode-select --install", repairCommand: "sudo xcode-select -s /Applications/Xcode.app/Contents/Developer", uninstallCommand: nil, isProtected: true)
        } else {
            prereqs.xcode = DependencyStatus(id: "xcode", name: "Xcode & Command Line Tools", isAvailable: false, details: "Not Installed", recommendation: "Install Apple Command Line Tools via 'xcode-select --install'", installCommand: "xcode-select --install", repairCommand: "xcode-select --reset", uninstallCommand: nil, isProtected: true)
        }
        
        func findBinary(_ name: String) -> String? {
            let search = ["/opt/homebrew/bin/\(name)", "/usr/local/bin/\(name)", "/usr/bin/\(name)"]
            for path in search where fm.fileExists(atPath: path) {
                return path
            }
            return nil
        }
        
        // 2. Homebrew Package Manager
        let isHomebrewInstalled: Bool
        if let brewPath = findBinary("brew") {
            isHomebrewInstalled = true
            let brewVer = runQuickCommand(brewPath, args: ["--version"])
            let firstLine = brewVer.output.components(separatedBy: .newlines).first ?? brewVer.output
            prereqs.homebrew = DependencyStatus(
                id: "homebrew",
                name: "Homebrew Package Manager",
                isAvailable: true,
                details: "\(firstLine) (\(brewPath))",
                recommendation: nil,
                installCommand: prereqs.homebrew.installCommand,
                repairCommand: "brew update",
                uninstallCommand: nil,
                isProtected: true
            )
        } else {
            isHomebrewInstalled = false
            prereqs.homebrew = DependencyStatus(
                id: "homebrew",
                name: "Homebrew Package Manager",
                isAvailable: false,
                details: "Not Installed",
                recommendation: "Required to install and manage compilers and tools. Click Install to set up automatically.",
                installCommand: prereqs.homebrew.installCommand,
                repairCommand: "brew update",
                uninstallCommand: nil,
                isProtected: true
            )
        }
        
        // 3. Git Version Control
        if let gitPath = findBinary("git") {
            let gitVer = runQuickCommand(gitPath, args: ["--version"])
            prereqs.git = DependencyStatus(id: "git", name: "Git Version Control", isAvailable: true, details: "\(gitVer.output) (\(gitPath))", recommendation: nil, installCommand: "brew install git", repairCommand: "brew reinstall git", uninstallCommand: nil, isProtected: true)
        } else {
            let rec = isHomebrewInstalled ? "Install Git via Homebrew or Xcode CLT" : "Install Homebrew first to install Git"
            prereqs.git = DependencyStatus(id: "git", name: "Git Version Control", isAvailable: false, details: "Not Found", recommendation: rec, installCommand: "brew install git", repairCommand: "brew reinstall git", uninstallCommand: nil, isProtected: true)
        }
        
        // 4. Python 3 (Python 3.12+ required by NxNative for direct RomFS image builds)
        func checkPythonVersion(_ path: String) -> (isSufficient: Bool, isStable: Bool, version: String) {
            let verOutput = runQuickCommand(path, args: ["-c", "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}')"])
            guard verOutput.success else {
                return (false, false, "Unknown")
            }
            let verStr = verOutput.output.trimmingCharacters(in: .whitespacesAndNewlines)
            let parts = verStr.split(separator: ".").compactMap { Int($0) }
            if parts.count >= 2 {
                let major = parts[0]
                let minor = parts[1]
                let isOk = (major > 3) || (major == 3 && minor >= 12)
                let isStable = (major == 3 && minor >= 12 && minor <= 14)
                return (isOk, isStable, "Python \(verStr)")
            }
            return (false, false, "Python \(verStr)")
        }
        
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
        var chosenPyPath: String? = nil
        var chosenPyVersion = ""
        var isPySufficient = false
        
        // 1. Prefer a Python that is verified stable (>= 3.12 and <= 3.14)
        for candidate in pyCandidates where fm.fileExists(atPath: candidate) {
            let check = checkPythonVersion(candidate)
            if check.isSufficient && check.isStable {
                chosenPyPath = candidate
                chosenPyVersion = check.version
                isPySufficient = true
                break
            }
        }
        
        // 2. If no stable 3.12-3.14 is found, accept any >= 3.12 (e.g. 3.15+)
        if chosenPyPath == nil {
            for candidate in pyCandidates where fm.fileExists(atPath: candidate) {
                let check = checkPythonVersion(candidate)
                if check.isSufficient {
                    chosenPyPath = candidate
                    chosenPyVersion = check.version
                    isPySufficient = true
                    break
                }
            }
        }
        
        // 3. If still none >= 3.12, pick the first existing python for diagnostic reporting
        if chosenPyPath == nil {
            for candidate in pyCandidates where fm.fileExists(atPath: candidate) {
                let check = checkPythonVersion(candidate)
                chosenPyPath = candidate
                chosenPyVersion = check.version
                isPySufficient = false
                break
            }
        }
        
        let pipInstallCmd = "PY_BIN=\"\"; for c in /opt/homebrew/opt/python@3.14/bin/python3 /opt/homebrew/opt/python@3.13/bin/python3 /opt/homebrew/opt/python@3.12/bin/python3 /opt/homebrew/bin/python3 /usr/local/opt/python@3.14/bin/python3 /usr/local/opt/python@3.13/bin/python3 /usr/local/opt/python@3.12/bin/python3 /usr/local/bin/python3 $(which python3); do if [ -x \"$c\" ]; then PY_BIN=\"$c\"; break; fi; done; if [ -n \"$PY_BIN\" ]; then if \"$PY_BIN\" -m pip install --help 2>&1 | grep -q -- '--break-system-packages'; then \"$PY_BIN\" -m pip install --break-system-packages lz4 capstone fonttools && (\"$PY_BIN\" -m pip install --break-system-packages nsz 2>/dev/null || true); else \"$PY_BIN\" -m pip install lz4 capstone fonttools && (\"$PY_BIN\" -m pip install nsz 2>/dev/null || true); fi; fi"
        let pipRepairCmd = "PY_BIN=\"\"; for c in /opt/homebrew/opt/python@3.14/bin/python3 /opt/homebrew/opt/python@3.13/bin/python3 /opt/homebrew/opt/python@3.12/bin/python3 /opt/homebrew/bin/python3 /usr/local/opt/python@3.14/bin/python3 /usr/local/opt/python@3.13/bin/python3 /usr/local/opt/python@3.12/bin/python3 /usr/local/bin/python3 $(which python3); do if [ -x \"$c\" ]; then PY_BIN=\"$c\"; break; fi; done; if [ -n \"$PY_BIN\" ]; then if \"$PY_BIN\" -m pip install --help 2>&1 | grep -q -- '--break-system-packages'; then \"$PY_BIN\" -m pip install --force-reinstall --break-system-packages lz4 capstone fonttools && (\"$PY_BIN\" -m pip install --force-reinstall --break-system-packages nsz 2>/dev/null || true); else \"$PY_BIN\" -m pip install --force-reinstall lz4 capstone fonttools && (\"$PY_BIN\" -m pip install --force-reinstall nsz 2>/dev/null || true); fi; fi"
        let pipUninstallCmd = "PY_BIN=\"\"; for c in /opt/homebrew/opt/python@3.14/bin/python3 /opt/homebrew/opt/python@3.13/bin/python3 /opt/homebrew/opt/python@3.12/bin/python3 /opt/homebrew/bin/python3 /usr/local/opt/python@3.14/bin/python3 /usr/local/opt/python@3.13/bin/python3 /usr/local/opt/python@3.12/bin/python3 /usr/local/bin/python3 $(which python3); do if [ -x \"$c\" ]; then PY_BIN=\"$c\"; break; fi; done; if [ -n \"$PY_BIN\" ]; then if \"$PY_BIN\" -m pip uninstall --help 2>&1 | grep -q -- '--break-system-packages'; then \"$PY_BIN\" -m pip uninstall -y --break-system-packages lz4 capstone fonttools nsz 2>/dev/null || true; else \"$PY_BIN\" -m pip uninstall -y lz4 capstone fonttools nsz 2>/dev/null || true; fi; fi"

        if let pyPath = chosenPyPath, isPySufficient {
            prereqs.python = DependencyStatus(
                id: "python",
                name: "Python 3",
                isAvailable: true,
                details: "\(chosenPyVersion) (\(pyPath))",
                recommendation: nil,
                installCommand: "brew install python@3.13",
                repairCommand: "brew reinstall python@3.13",
                uninstallCommand: nil,
                isProtected: false
            )
            
            // Python modules: core modules are lz4, capstone, fontTools
            let pyCheck = runQuickCommand(pyPath, args: ["-c", "import lz4, capstone, fontTools; print('OK')"])
            if pyCheck.success && pyCheck.output.contains("OK") {
                let nszCheck = runQuickCommand(pyPath, args: ["-c", "import nsz; print('OK')"])
                let nszNote = nszCheck.success ? " + nsz" : " (nsz optional)"
                prereqs.pythonPackages = DependencyStatus(
                    id: "python_packages",
                    name: "Python Modules",
                    isAvailable: true,
                    details: "lz4, capstone, fonttools\(nszNote) installed",
                    recommendation: nszCheck.success ? nil : "Note: Compressed .nsz/.xcz files require standalone nsz or Python <= 3.14",
                    installCommand: pipInstallCmd,
                    repairCommand: pipRepairCmd,
                    uninstallCommand: pipUninstallCmd,
                    isProtected: false
                )
            } else {
                prereqs.pythonPackages = DependencyStatus(
                    id: "python_packages",
                    name: "Python Modules",
                    isAvailable: false,
                    details: "Missing required packages for \(chosenPyVersion)",
                    recommendation: "Install packages via pip",
                    installCommand: pipInstallCmd,
                    repairCommand: pipRepairCmd,
                    uninstallCommand: pipUninstallCmd,
                    isProtected: false
                )
            }
        } else if let pyPath = chosenPyPath {
            let rec = isHomebrewInstalled ? "Python 3.12+ is required. Click Install to install modern Python via Homebrew." : "Install Homebrew first, then install Python 3.12+."
            prereqs.python = DependencyStatus(
                id: "python",
                name: "Python 3",
                isAvailable: false,
                details: "\(chosenPyVersion) (\(pyPath)) - Python 3.12+ required",
                recommendation: rec,
                installCommand: "brew install python",
                repairCommand: "brew reinstall python",
                uninstallCommand: nil,
                isProtected: false
            )
            prereqs.pythonPackages = DependencyStatus(
                id: "python_packages",
                name: "Python Modules",
                isAvailable: false,
                details: "Requires Python 3.12+",
                recommendation: isHomebrewInstalled ? "Install Python 3 first via Homebrew" : "Install Homebrew first",
                installCommand: pipInstallCmd,
                repairCommand: pipRepairCmd,
                uninstallCommand: pipUninstallCmd,
                isProtected: false
            )
        } else {
            let rec = isHomebrewInstalled ? "Install Python 3 via Homebrew" : "Install Homebrew first to install Python 3"
            prereqs.python = DependencyStatus(
                id: "python",
                name: "Python 3",
                isAvailable: false,
                details: "Not Found",
                recommendation: rec,
                installCommand: "brew install python",
                repairCommand: "brew reinstall python",
                uninstallCommand: nil,
                isProtected: false
            )
            prereqs.pythonPackages = DependencyStatus(
                id: "python_packages",
                name: "Python Modules",
                isAvailable: false,
                details: "Python 3 Not Found",
                recommendation: isHomebrewInstalled ? "Install Python 3 first via Homebrew" : "Install Homebrew first",
                installCommand: pipInstallCmd,
                repairCommand: pipRepairCmd,
                uninstallCommand: pipUninstallCmd,
                isProtected: false
            )
        }
        
        // 5. .NET SDK
        if let dotnetPath = findBinary("dotnet") {
            let dotnetVer = runQuickCommand(dotnetPath, args: ["--version"])
            prereqs.dotnet = DependencyStatus(id: "dotnet", name: ".NET SDK", isAvailable: true, details: "v\(dotnetVer.output)", recommendation: nil, installCommand: "brew install --cask dotnet-sdk", repairCommand: "brew reinstall --cask dotnet-sdk", uninstallCommand: "brew uninstall --cask dotnet-sdk", isProtected: false)
        } else {
            let rec = isHomebrewInstalled ? "Install .NET SDK via Homebrew" : "Install Homebrew first to install .NET SDK"
            prereqs.dotnet = DependencyStatus(id: "dotnet", name: ".NET SDK", isAvailable: false, details: "Not Found", recommendation: rec, installCommand: "brew install --cask dotnet-sdk", repairCommand: "brew reinstall --cask dotnet-sdk", uninstallCommand: "brew uninstall --cask dotnet-sdk", isProtected: false)
        }
        
        // 6. XcodeGen
        if let xcodegenPath = findBinary("xcodegen") {
            let xcodegenVer = runQuickCommand(xcodegenPath, args: ["--version"])
            prereqs.xcodegen = DependencyStatus(id: "xcodegen", name: "XcodeGen", isAvailable: true, details: xcodegenVer.output, recommendation: nil, installCommand: "brew install xcodegen", repairCommand: "brew reinstall xcodegen", uninstallCommand: "brew uninstall xcodegen", isProtected: false)
        } else {
            let rec = isHomebrewInstalled ? "Install XcodeGen via Homebrew" : "Install Homebrew first to install XcodeGen"
            prereqs.xcodegen = DependencyStatus(id: "xcodegen", name: "XcodeGen", isAvailable: false, details: "Not Found", recommendation: rec, installCommand: "brew install xcodegen", repairCommand: "brew reinstall xcodegen", uninstallCommand: "brew uninstall xcodegen", isProtected: false)
        }
        
        // 7. zstd
        if let zstdPath = findBinary("zstd") {
            prereqs.zstd = DependencyStatus(id: "zstd", name: "zstd", isAvailable: true, details: "Installed (\(zstdPath))", recommendation: nil, installCommand: "brew install zstd", repairCommand: "brew reinstall zstd", uninstallCommand: "brew uninstall zstd", isProtected: false)
        } else {
            let rec = isHomebrewInstalled ? "Install zstd via Homebrew" : "Install Homebrew first to install zstd"
            prereqs.zstd = DependencyStatus(id: "zstd", name: "zstd", isAvailable: false, details: "Not Found", recommendation: rec, installCommand: "brew install zstd", repairCommand: "brew reinstall zstd", uninstallCommand: "brew uninstall zstd", isProtected: false)
        }
        
        // 8. spirv-cross
        if let spirvPath = findBinary("spirv-cross") {
            prereqs.spirvCross = DependencyStatus(id: "spirv_cross", name: "spirv-cross", isAvailable: true, details: "Installed (\(spirvPath))", recommendation: nil, installCommand: "brew install spirv-cross", repairCommand: "brew reinstall spirv-cross", uninstallCommand: "brew uninstall spirv-cross", isProtected: false)
        } else {
            let rec = isHomebrewInstalled ? "Install spirv-cross via Homebrew" : "Install Homebrew first to install spirv-cross"
            prereqs.spirvCross = DependencyStatus(id: "spirv_cross", name: "spirv-cross", isAvailable: false, details: "Not Found", recommendation: rec, installCommand: "brew install spirv-cross", repairCommand: "brew reinstall spirv-cross", uninstallCommand: "brew uninstall spirv-cross", isProtected: false)
        }
        
        // 9. glslangValidator
        if let glslangPath = findBinary("glslangValidator") {
            prereqs.glslang = DependencyStatus(id: "glslang", name: "glslang", isAvailable: true, details: "Installed (\(glslangPath))", recommendation: nil, installCommand: "brew install glslang", repairCommand: "brew reinstall glslang", uninstallCommand: "brew uninstall glslang", isProtected: false)
        } else {
            let rec = isHomebrewInstalled ? "Install glslang via Homebrew" : "Install Homebrew first to install glslang"
            prereqs.glslang = DependencyStatus(id: "glslang", name: "glslang", isAvailable: false, details: "Not Found", recommendation: rec, installCommand: "brew install glslang", repairCommand: "brew reinstall glslang", uninstallCommand: "brew uninstall glslang", isProtected: false)
        }
        
        return prereqs
    }
}
