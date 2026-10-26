import Foundation

public struct GameRuntimeConfig: Codable, Equatable {
    public var enable60FPS: Bool
    public var enableMetalHUD: Bool
    public var isDockedMode: Bool // true = Docked (1080p), false = Handheld (720p)
    public var renderScale: Double // 0.5 to 1.0
    public var enableMetalFXUpscale: Bool // true = MetalFX, false = off
    
    public init(
        enable60FPS: Bool = true,
        enableMetalHUD: Bool = false,
        isDockedMode: Bool = true,
        renderScale: Double = 1.0,
        enableMetalFXUpscale: Bool = false
    ) {
        self.enable60FPS = enable60FPS
        self.enableMetalHUD = enableMetalHUD
        self.isDockedMode = isDockedMode
        self.renderScale = renderScale
        self.enableMetalFXUpscale = enableMetalFXUpscale
    }
    
    public func toEnvironmentDict() -> [String: String] {
        return [
            "DM_FPS_TARGET": enable60FPS ? "60" : "30",
            "DM_METAL_HUD": enableMetalHUD ? "1" : "0",
            "DM_OPMODE": isDockedMode ? "1" : "0",
            "DM_RENDER_SCALE": String(format: "%.2f", renderScale),
            "DM_PRESENT_UPSCALE": enableMetalFXUpscale ? "metalfx" : "off"
        ]
    }
    
    public func envFileContent() -> String {
        return """
        # nxnative runtime configuration
        DM_FPS_TARGET=\(enable60FPS ? "60" : "30")
        DM_METAL_HUD=\(enableMetalHUD ? "1" : "0")
        DM_OPMODE=\(isDockedMode ? "1" : "0")
        DM_RENDER_SCALE=\(String(format: "%.2f", renderScale))
        DM_PRESENT_UPSCALE=\(enableMetalFXUpscale ? "metalfx" : "off")
        """
    }
    
    public static func storageKey(for game: PortedGame) -> String {
        return "NXNative_GameConfig_" + game.uniqueKey
    }
    
    public static func load(for game: PortedGame) -> GameRuntimeConfig {
        let key = storageKey(for: game)
        if let data = UserDefaults.standard.data(forKey: key),
           let config = try? JSONDecoder().decode(GameRuntimeConfig.self, from: data) {
            return config
        }
        
        // Also check if an existing env.txt exists in Application Support or bundle
        if let titleId = game.titleId, !titleId.isEmpty {
            let appSup = FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Library/Application Support/nxnative")
                .appendingPathComponent(titleId)
                .appendingPathComponent("env.txt")
            if let content = try? String(contentsOf: appSup, encoding: .utf8) {
                return parseEnvContent(content)
            }
        }
        
        let bundleEnv = game.url.appendingPathComponent("Contents/Resources/dm/env.txt")
        if let content = try? String(contentsOf: bundleEnv, encoding: .utf8) {
            return parseEnvContent(content)
        }
        
        return GameRuntimeConfig()
    }
    
    public func save(for game: PortedGame) {
        let key = GameRuntimeConfig.storageKey(for: game)
        if let data = try? JSONEncoder().encode(self) {
            UserDefaults.standard.set(data, forKey: key)
        }
        
        applyToDisk(for: game)
    }
    
    public func applyToDisk(for game: PortedGame) {
        let content = envFileContent()
        let fm = FileManager.default
        
        // 1. Write to ~/Library/Application Support/nxnative/<titleId>/env.txt
        if let titleId = game.titleId, !titleId.isEmpty {
            let appSupDir = fm.homeDirectoryForCurrentUser
                .appendingPathComponent("Library/Application Support/nxnative")
                .appendingPathComponent(titleId)
            try? fm.createDirectory(at: appSupDir, withIntermediateDirectories: true)
            let envFile = appSupDir.appendingPathComponent("env.txt")
            try? content.write(to: envFile, atomically: true, encoding: .utf8)
        }
        
        // 2. Also write to <Game.app>/Contents/Resources/dm/env.txt
        let dmDir = game.url.appendingPathComponent("Contents/Resources/dm")
        if fm.fileExists(atPath: dmDir.path) {
            let bundleEnv = dmDir.appendingPathComponent("env.txt")
            try? content.write(to: bundleEnv, atomically: true, encoding: .utf8)
        }
    }
    
    private static func parseEnvContent(_ text: String) -> GameRuntimeConfig {
        var config = GameRuntimeConfig()
        let lines = text.components(separatedBy: .newlines)
        for line in lines {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("#") || !trimmed.contains("=") { continue }
            let parts = trimmed.split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { continue }
            let key = parts[0].trimmingCharacters(in: .whitespaces)
            let val = parts[1].trimmingCharacters(in: .whitespaces)
            
            switch key {
            case "DM_FPS_TARGET":
                config.enable60FPS = (val == "60")
            case "DM_METAL_HUD":
                config.enableMetalHUD = (val == "1")
            case "DM_OPMODE":
                config.isDockedMode = (val != "0")
            case "DM_RENDER_SCALE":
                if let scale = Double(val), scale >= 0.5 && scale <= 1.0 {
                    config.renderScale = scale
                }
            case "DM_PRESENT_UPSCALE":
                config.enableMetalFXUpscale = (val.lowercased() == "metalfx" || val == "1")
            default:
                break
            }
        }
        return config
    }
}
