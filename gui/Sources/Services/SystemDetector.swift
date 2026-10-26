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
        
        // 2. Python 3
        if let pyPath = findBinary("python3") {
            let pyVer = runQuickCommand(pyPath, args: ["--version"])
            prereqs.python = DependencyStatus(id: "python", name: "Python 3", isAvailable: true, details: "\(pyVer.output) (\(pyPath))", recommendation: nil, installCommand: "brew install python", repairCommand: "brew reinstall python", uninstallCommand: nil, isProtected: true)
            
            // Python modules
            let pyCheck = runQuickCommand(pyPath, args: ["-c", "import lz4, capstone, fontTools; print('OK')"])
            if pyCheck.success && pyCheck.output.contains("OK") {
                let nszCheck = runQuickCommand(pyPath, args: ["-c", "import nsz; print('OK')"])
                let nszNote = nszCheck.success ? " + nsz" : ""
                prereqs.pythonPackages = DependencyStatus(id: "python_packages", name: "Python Modules", isAvailable: true, details: "lz4, capstone, fonttools\(nszNote) installed", recommendation: nil, installCommand: "\(pyPath) -m pip install --break-system-packages lz4 capstone fonttools nsz", repairCommand: "\(pyPath) -m pip install --force-reinstall --break-system-packages lz4 capstone fonttools nsz", uninstallCommand: "\(pyPath) -m pip uninstall -y --break-system-packages lz4 capstone fonttools nsz", isProtected: false)
            } else {
                prereqs.pythonPackages = DependencyStatus(id: "python_packages", name: "Python Modules", isAvailable: false, details: "Missing required packages", recommendation: "Install packages via pip", installCommand: "\(pyPath) -m pip install --break-system-packages lz4 capstone fonttools nsz", repairCommand: "\(pyPath) -m pip install --force-reinstall --break-system-packages lz4 capstone fonttools nsz", uninstallCommand: "\(pyPath) -m pip uninstall -y --break-system-packages lz4 capstone fonttools nsz", isProtected: false)
            }
        } else {
            prereqs.python = DependencyStatus(id: "python", name: "Python 3", isAvailable: false, details: "Not Found", recommendation: "Install Python 3 via Homebrew", installCommand: "brew install python", repairCommand: "brew reinstall python", uninstallCommand: nil, isProtected: true)
            prereqs.pythonPackages = DependencyStatus(id: "python_packages", name: "Python Modules", isAvailable: false, details: "Python Not Found", recommendation: nil, installCommand: "python3 -m pip install --break-system-packages lz4 capstone fonttools nsz", repairCommand: "python3 -m pip install --force-reinstall --break-system-packages lz4 capstone fonttools nsz", uninstallCommand: nil, isProtected: false)
        }
        
        // 3. .NET SDK
        if let dotnetPath = findBinary("dotnet") {
            let dotnetVer = runQuickCommand(dotnetPath, args: ["--version"])
            prereqs.dotnet = DependencyStatus(id: "dotnet", name: ".NET SDK", isAvailable: true, details: "v\(dotnetVer.output)", recommendation: nil, installCommand: "brew install --cask dotnet-sdk", repairCommand: "brew reinstall --cask dotnet-sdk", uninstallCommand: "brew uninstall --cask dotnet-sdk", isProtected: false)
        } else {
            prereqs.dotnet = DependencyStatus(id: "dotnet", name: ".NET SDK", isAvailable: false, details: "Not Found", recommendation: "Install .NET SDK via Homebrew", installCommand: "brew install --cask dotnet-sdk", repairCommand: "brew reinstall --cask dotnet-sdk", uninstallCommand: "brew uninstall --cask dotnet-sdk", isProtected: false)
        }
        
        // 4. XcodeGen
        if let xcodegenPath = findBinary("xcodegen") {
            let xcodegenVer = runQuickCommand(xcodegenPath, args: ["--version"])
            prereqs.xcodegen = DependencyStatus(id: "xcodegen", name: "XcodeGen", isAvailable: true, details: xcodegenVer.output, recommendation: nil, installCommand: "brew install xcodegen", repairCommand: "brew reinstall xcodegen", uninstallCommand: "brew uninstall xcodegen", isProtected: false)
        } else {
            prereqs.xcodegen = DependencyStatus(id: "xcodegen", name: "XcodeGen", isAvailable: false, details: "Not Found", recommendation: "Install XcodeGen via Homebrew", installCommand: "brew install xcodegen", repairCommand: "brew reinstall xcodegen", uninstallCommand: "brew uninstall xcodegen", isProtected: false)
        }
        
        // 5. zstd
        if let zstdPath = findBinary("zstd") {
            prereqs.zstd = DependencyStatus(id: "zstd", name: "zstd", isAvailable: true, details: "Installed (\(zstdPath))", recommendation: nil, installCommand: "brew install zstd", repairCommand: "brew reinstall zstd", uninstallCommand: "brew uninstall zstd", isProtected: false)
        } else {
            prereqs.zstd = DependencyStatus(id: "zstd", name: "zstd", isAvailable: false, details: "Not Found", recommendation: "Install zstd via Homebrew", installCommand: "brew install zstd", repairCommand: "brew reinstall zstd", uninstallCommand: "brew uninstall zstd", isProtected: false)
        }
        
        // 6. spirv-cross
        if let spirvPath = findBinary("spirv-cross") {
            prereqs.spirvCross = DependencyStatus(id: "spirv_cross", name: "spirv-cross", isAvailable: true, details: "Installed (\(spirvPath))", recommendation: nil, installCommand: "brew install spirv-cross", repairCommand: "brew reinstall spirv-cross", uninstallCommand: "brew uninstall spirv-cross", isProtected: false)
        } else {
            prereqs.spirvCross = DependencyStatus(id: "spirv_cross", name: "spirv-cross", isAvailable: false, details: "Not Found", recommendation: "Install spirv-cross via Homebrew", installCommand: "brew install spirv-cross", repairCommand: "brew reinstall spirv-cross", uninstallCommand: "brew uninstall spirv-cross", isProtected: false)
        }
        
        // 7. glslangValidator
        if let glslangPath = findBinary("glslangValidator") {
            prereqs.glslang = DependencyStatus(id: "glslang", name: "glslang", isAvailable: true, details: "Installed (\(glslangPath))", recommendation: nil, installCommand: "brew install glslang", repairCommand: "brew reinstall glslang", uninstallCommand: "brew uninstall glslang", isProtected: false)
        } else {
            prereqs.glslang = DependencyStatus(id: "glslang", name: "glslang", isAvailable: false, details: "Not Found", recommendation: "Install glslang via Homebrew", installCommand: "brew install glslang", repairCommand: "brew reinstall glslang", uninstallCommand: "brew uninstall glslang", isProtected: false)
        }
        
        return prereqs
    }
}
