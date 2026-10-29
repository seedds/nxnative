import Foundation

public struct DependencyStatus: Identifiable, Equatable {
    public let id: String
    public let name: String
    public var isAvailable: Bool
    public var details: String
    public var recommendation: String?
    public var installCommand: String?
    public var repairCommand: String?
    public var uninstallCommand: String?
    public var isProtected: Bool // Core system tools cannot be uninstalled
    public var isInstalling: Bool = false
    
    public init(
        id: String,
        name: String,
        isAvailable: Bool,
        details: String,
        recommendation: String? = nil,
        installCommand: String? = nil,
        repairCommand: String? = nil,
        uninstallCommand: String? = nil,
        isProtected: Bool = false,
        isInstalling: Bool = false
    ) {
        self.id = id
        self.name = name
        self.isAvailable = isAvailable
        self.details = details
        self.recommendation = recommendation
        self.installCommand = installCommand
        self.repairCommand = repairCommand
        self.uninstallCommand = uninstallCommand
        self.isProtected = isProtected
        self.isInstalling = isInstalling
    }
}

public struct SystemPrerequisites {
    public var xcode: DependencyStatus = DependencyStatus(
        id: "xcode",
        name: "Xcode & Command Line Tools",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "xcode-select --install",
        repairCommand: "xcode-select --reset",
        uninstallCommand: nil,
        isProtected: true
    )
    
    public var homebrew: DependencyStatus = DependencyStatus(
        id: "homebrew",
        name: "Homebrew Package Manager",
        isAvailable: false,
        details: "Checking...",
        recommendation: "Required to install and manage compilers, SDKs, and build tools",
        installCommand: "NONINTERACTIVE=1 /bin/bash -c \"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\" && (if [ -x /opt/homebrew/bin/brew ]; then eval \"$(/opt/homebrew/bin/brew shellenv)\"; grep -q 'brew shellenv' ~/.zprofile 2>/dev/null || echo 'eval \"$(/opt/homebrew/bin/brew shellenv)\"' >> ~/.zprofile; elif [ -x /usr/local/bin/brew ]; then eval \"$(/usr/local/bin/brew shellenv)\"; grep -q 'brew shellenv' ~/.zprofile 2>/dev/null || echo 'eval \"$(/usr/local/bin/brew shellenv)\"' >> ~/.zprofile; fi)",
        repairCommand: "brew update",
        uninstallCommand: nil,
        isProtected: true
    )
    
    public var git: DependencyStatus = DependencyStatus(
        id: "git",
        name: "Git Version Control",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "brew install git",
        repairCommand: "brew reinstall git",
        uninstallCommand: nil,
        isProtected: true
    )
    
    public var python: DependencyStatus = DependencyStatus(
        id: "python",
        name: "Python 3",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "brew install python",
        repairCommand: "brew reinstall python",
        uninstallCommand: nil,
        isProtected: false
    )
    
    public var dotnet: DependencyStatus = DependencyStatus(
        id: "dotnet",
        name: ".NET SDK",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "brew install --cask dotnet-sdk",
        repairCommand: "brew reinstall --cask dotnet-sdk",
        uninstallCommand: "brew uninstall --cask dotnet-sdk",
        isProtected: false
    )
    
    public var xcodegen: DependencyStatus = DependencyStatus(
        id: "xcodegen",
        name: "XcodeGen",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "brew install xcodegen",
        repairCommand: "brew reinstall xcodegen",
        uninstallCommand: "brew uninstall xcodegen",
        isProtected: false
    )
    
    public var zstd: DependencyStatus = DependencyStatus(
        id: "zstd",
        name: "zstd",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "brew install zstd",
        repairCommand: "brew reinstall zstd",
        uninstallCommand: "brew uninstall zstd",
        isProtected: false
    )
    
    public var spirvCross: DependencyStatus = DependencyStatus(
        id: "spirv_cross",
        name: "spirv-cross",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "brew install spirv-cross",
        repairCommand: "brew reinstall spirv-cross",
        uninstallCommand: "brew uninstall spirv-cross",
        isProtected: false
    )
    
    public var glslang: DependencyStatus = DependencyStatus(
        id: "glslang",
        name: "glslang",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "brew install glslang",
        repairCommand: "brew reinstall glslang",
        uninstallCommand: "brew uninstall glslang",
        isProtected: false
    )
    
    public var pythonPackages: DependencyStatus = DependencyStatus(
        id: "python_packages",
        name: "Python Modules",
        isAvailable: false,
        details: "Checking...",
        recommendation: nil,
        installCommand: "PY_BIN=\"\"; for c in /opt/homebrew/opt/python@3.14/bin/python3 /opt/homebrew/opt/python@3.13/bin/python3 /opt/homebrew/opt/python@3.12/bin/python3 /opt/homebrew/bin/python3 /usr/local/opt/python@3.14/bin/python3 /usr/local/opt/python@3.13/bin/python3 /usr/local/opt/python@3.12/bin/python3 /usr/local/bin/python3 $(which python3); do if [ -x \"$c\" ]; then PY_BIN=\"$c\"; break; fi; done; if [ -n \"$PY_BIN\" ]; then if \"$PY_BIN\" -m pip install --help 2>&1 | grep -q -- '--break-system-packages'; then \"$PY_BIN\" -m pip install --break-system-packages lz4 capstone fonttools && (\"$PY_BIN\" -m pip install --break-system-packages nsz 2>/dev/null || true); else \"$PY_BIN\" -m pip install lz4 capstone fonttools && (\"$PY_BIN\" -m pip install nsz 2>/dev/null || true); fi; fi",
        repairCommand: "PY_BIN=\"\"; for c in /opt/homebrew/opt/python@3.14/bin/python3 /opt/homebrew/opt/python@3.13/bin/python3 /opt/homebrew/opt/python@3.12/bin/python3 /opt/homebrew/bin/python3 /usr/local/opt/python@3.14/bin/python3 /usr/local/opt/python@3.13/bin/python3 /usr/local/opt/python@3.12/bin/python3 /usr/local/bin/python3 $(which python3); do if [ -x \"$c\" ]; then PY_BIN=\"$c\"; break; fi; done; if [ -n \"$PY_BIN\" ]; then if \"$PY_BIN\" -m pip install --help 2>&1 | grep -q -- '--break-system-packages'; then \"$PY_BIN\" -m pip install --force-reinstall --break-system-packages lz4 capstone fonttools && (\"$PY_BIN\" -m pip install --force-reinstall --break-system-packages nsz 2>/dev/null || true); else \"$PY_BIN\" -m pip install --force-reinstall lz4 capstone fonttools && (\"$PY_BIN\" -m pip install --force-reinstall nsz 2>/dev/null || true); fi; fi",
        uninstallCommand: "PY_BIN=\"\"; for c in /opt/homebrew/opt/python@3.14/bin/python3 /opt/homebrew/opt/python@3.13/bin/python3 /opt/homebrew/opt/python@3.12/bin/python3 /opt/homebrew/bin/python3 /usr/local/opt/python@3.14/bin/python3 /usr/local/opt/python@3.13/bin/python3 /usr/local/opt/python@3.12/bin/python3 /usr/local/bin/python3 $(which python3); do if [ -x \"$c\" ]; then PY_BIN=\"$c\"; break; fi; done; if [ -n \"$PY_BIN\" ]; then if \"$PY_BIN\" -m pip uninstall --help 2>&1 | grep -q -- '--break-system-packages'; then \"$PY_BIN\" -m pip uninstall -y --break-system-packages lz4 capstone fonttools nsz 2>/dev/null || true; else \"$PY_BIN\" -m pip uninstall -y lz4 capstone fonttools nsz 2>/dev/null || true; fi; fi",
        isProtected: false
    )
    
    public var allSatisfied: Bool {
        xcode.isAvailable &&
        homebrew.isAvailable &&
        git.isAvailable &&
        python.isAvailable &&
        dotnet.isAvailable &&
        xcodegen.isAvailable &&
        zstd.isAvailable &&
        spirvCross.isAvailable &&
        glslang.isAvailable &&
        pythonPackages.isAvailable
    }
    
    public var missingList: [DependencyStatus] {
        list.filter { !$0.isAvailable }
    }
    
    public var list: [DependencyStatus] {
        [xcode, homebrew, git, python, pythonPackages, dotnet, xcodegen, zstd, spirvCross, glslang]
    }
}
