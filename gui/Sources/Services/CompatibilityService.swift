import Foundation

public final class CompatibilityService {
    public static let shared = CompatibilityService()
    
    private let githubRawURL = URL(string: "https://raw.githubusercontent.com/seedds/nxnative/main/dm/games/README.md")!
    public private(set) var cachedGames: [CompatibleGameItem] = []
    
    private init() {}
    
    public func fetchCompatibleGames(
        localNxnativePath: String? = nil,
        completion: @escaping ([CompatibleGameItem], Bool) -> Void
    ) {
        // Try network fetch first
        var request = URLRequest(url: githubRawURL)
        request.timeoutInterval = 7.0
        request.cachePolicy = .reloadIgnoringLocalCacheData
        
        let task = URLSession.shared.dataTask(with: request) { [weak self] data, response, error in
            guard let self = self else { return }
            
            if let d = data, let text = String(data: d, encoding: .utf8), !text.isEmpty {
                let games = self.parseMarkdownTable(text)
                if !games.isEmpty {
                    DispatchQueue.main.async {
                        completion(games, true) // Online
                    }
                    return
                }
            }
            
            // Fallback to local README.md
            self.loadFromLocal(nxnativePath: localNxnativePath) { localGames in
                DispatchQueue.main.async {
                    completion(localGames, false) // Offline cache
                }
            }
        }
        task.resume()
    }
    
    public func loadFromLocal(nxnativePath: String?, completion: @escaping ([CompatibleGameItem]) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            var candidatePaths: [String] = []
            
            if let p = nxnativePath, !p.isEmpty {
                candidatePaths.append(URL(fileURLWithPath: p).appendingPathComponent("dm/games/README.md").path)
            }
            candidatePaths.append("/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/dm/games/README.md")
            
            let fm = FileManager.default
            for path in candidatePaths {
                if fm.fileExists(atPath: path),
                   let text = try? String(contentsOfFile: path, encoding: .utf8) {
                    let items = self.parseMarkdownTable(text)
                    if !items.isEmpty {
                        completion(items)
                        return
                    }
                }
            }
            completion([])
        }
    }
    
    public func parseMarkdownTable(_ markdown: String) -> [CompatibleGameItem] {
        var items: [CompatibleGameItem] = []
        let pattern = #"^\|\s*\[([^\]]+)\]\(([^)]+)\)\s*\|\s*`?([0-9A-Fa-f]{16})`?\s*\|\s*([^|]+)\s*\|\s*([^|]+)\s*\|\s*([^|]+)\s*\|\s*([^|]+)\s*\|"#
        
        guard let regex = try? NSRegularExpression(pattern: pattern, options: [.anchorsMatchLines]) else {
            return []
        }
        
        let nsString = markdown as NSString
        let matches = regex.matches(in: markdown, options: [], range: NSRange(location: 0, length: nsString.length))
        
        for m in matches {
            guard m.numberOfRanges >= 8 else { continue }
            
            let title = nsString.substring(with: m.range(at: 1)).trimmingCharacters(in: .whitespaces)
            let path = nsString.substring(with: m.range(at: 2)).trimmingCharacters(in: .whitespaces)
            let tid = nsString.substring(with: m.range(at: 3)).trimmingCharacters(in: .whitespaces)
            let ver = nsString.substring(with: m.range(at: 4)).trimmingCharacters(in: .whitespaces)
            let sdk = nsString.substring(with: m.range(at: 5)).trimmingCharacters(in: .whitespaces)
            let engine = nsString.substring(with: m.range(at: 6)).trimmingCharacters(in: .whitespaces)
            let api = nsString.substring(with: m.range(at: 7)).trimmingCharacters(in: .whitespaces)
            
            let specs = self.resolveSpecs(titleId: tid, readmeRelPath: path, engine: engine)
            
            items.append(
                CompatibleGameItem(
                    title: title,
                    titleId: tid,
                    versionTested: ver,
                    sdk: sdk,
                    engine: engine,
                    graphicsApi: api,
                    readmeRelPath: path,
                    macReq: specs.mac,
                    iosReq: specs.ios,
                    storageReq: specs.storage,
                    minRamGB: specs.minRamGB
                )
            )
        }
        
        self.cachedGames = items
        return items
    }
    
    private func normalizeTitle(_ raw: String) -> String {
        let folded = raw.folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
        let stopWords = Set(["the", "of", "and", "edition", "collection", "remastered", "definitive", "complete", "reimagined", "asian", "release"])
        let words = folded
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
        let filtered = words.filter { !stopWords.contains($0) }
        let chosen = filtered.isEmpty ? words : filtered
        return chosen.joined()
    }
    
    public func findTitleId(forGameTitle cleanTitle: String) -> String? {
        let rawNorm = cleanTitle
            .folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
            .filter { $0.isLetter || $0.isNumber }
        let semanticNorm = normalizeTitle(cleanTitle)
        guard !rawNorm.isEmpty else { return nil }
        
        var list = cachedGames
        if list.isEmpty {
            let localPath = "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/dm/games/README.md"
            if let text = try? String(contentsOfFile: localPath, encoding: .utf8) {
                list = parseMarkdownTable(text)
            }
        }
        
        // 1. Exact folded match
        for game in list {
            let normGame = game.title
                .folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
                .filter { $0.isLetter || $0.isNumber }
            if normGame == rawNorm {
                return game.titleId
            }
        }
        
        // 2. Semantic word match (ignores "The", "of", word order variations like "Legend of Zelda The...")
        for game in list {
            let semGame = normalizeTitle(game.title)
            if !semGame.isEmpty && (semGame == semanticNorm || (semGame.contains(semanticNorm) && semanticNorm.count >= 5) || (semanticNorm.contains(semGame) && semGame.count >= 5)) {
                return game.titleId
            }
        }
        
        // 3. Substring match on folded characters
        for game in list {
            let normGame = game.title
                .folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
                .filter { $0.isLetter || $0.isNumber }
            if (normGame.contains(rawNorm) && rawNorm.count >= 5) || (rawNorm.contains(normGame) && normGame.count >= 5) {
                return game.titleId
            }
        }
        return nil
    }

    /// Resolves minimum Mac and iOS hardware specs (RAM, Storage, minRamGB tier: 4, 6, 8, or 16) from nxnative game profiles and README notes.
    private func resolveSpecs(titleId: String, readmeRelPath: String, engine: String = "") -> (mac: String, ios: String, storage: String, minRamGB: Int) {
        let baseDir = "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/dm/games/\(titleId)"
        let tsvPath = "\(baseDir)/game.tsv"
        let readmePath = "\(baseDir)/README.md"
        
        var memReserve: UInt64 = 0x100000000
        var appPool: UInt64 = 0xCD500000
        var sysResource: UInt64 = 0
        
        if let tsvContent = try? String(contentsOfFile: tsvPath, encoding: .utf8) {
            for line in tsvContent.components(separatedBy: .newlines) {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
                let parts = line.components(separatedBy: "\t")
                guard parts.count >= 2 else { continue }
                let key = parts[0].trimmingCharacters(in: .whitespaces)
                let valStr = parts[1].components(separatedBy: "#")[0].trimmingCharacters(in: .whitespaces)
                let cleanHex = valStr.lowercased().replacingOccurrences(of: "0x", with: "")
                if key == "mem_reserve", let v = UInt64(cleanHex, radix: 16) {
                    memReserve = v
                } else if key == "app_pool", let v = UInt64(cleanHex, radix: 16) {
                    appPool = v
                } else if key == "sys_resource" {
                    if let v = UInt64(cleanHex, radix: 16) {
                        sysResource = v
                    } else if let v = UInt64(valStr) {
                        sysResource = v
                    }
                }
            }
        }
        
        var storage = "HD: ~6 GB"
        var maxResidentGB: Double? = nil
        if let rmContent = try? String(contentsOfFile: readmePath, encoding: .utf8) {
            let storagePattern = #"(?:IPA|RomFS|app|game)[^\n\r]{0,35}?(?:at about|is about|about|is)?\s*([0-9]+(?:\.[0-9]+)?)\s*GB"#
            if let regex = try? NSRegularExpression(pattern: storagePattern, options: [.caseInsensitive]) {
                let ns = rmContent as NSString
                if let match = regex.firstMatch(in: rmContent, options: [], range: NSRange(location: 0, length: ns.length)), match.numberOfRanges >= 2 {
                    let gbVal = ns.substring(with: match.range(at: 1))
                    storage = "HD: ~\(gbVal) GB"
                }
            }
            
            let resPattern = #"([0-9]+(?:\.[0-9]+)?)\s*GB\s*(?:resident|of GPU)"#
            if let resRegex = try? NSRegularExpression(pattern: resPattern, options: [.caseInsensitive]) {
                let ns = rmContent as NSString
                let matches = resRegex.matches(in: rmContent, options: [], range: NSRange(location: 0, length: ns.length))
                for m in matches where m.numberOfRanges >= 2 {
                    if let val = Double(ns.substring(with: m.range(at: 1))) {
                        maxResidentGB = max(maxResidentGB ?? 0, val)
                    }
                }
            }
        }
        
        // Determine RAM tier: 4, 6, 8, or 16 GB
        let minRamGB: Int
        let isUnreal = engine.localizedCaseInsensitiveContains("unreal")
        
        if let res = maxResidentGB, res <= 1.35 {
            minRamGB = 4
        } else if appPool <= 0x40000000 || (memReserve < 0x100000000 && sysResource == 0) {
            minRamGB = 4
        } else if memReserve == 0x100000000 && sysResource == 0 {
            if let res = maxResidentGB, res >= 2.6 {
                minRamGB = 8
            } else if titleId == "01007EF00011E000" || titleId == "0100E95004038000" || titleId == "0100FF500E34A000" {
                // Breath of the Wild, Xenoblade 1 & 2
                minRamGB = 8
            } else {
                minRamGB = 6
            }
        } else if memReserve >= 0x140000000 || sysResource >= 0x2000000 || (isUnreal && sysResource > 0) {
            if appPool <= 0x80000000 {
                minRamGB = 8
            } else if let res = maxResidentGB, res <= 2.55 && !isUnreal {
                minRamGB = 8
            } else if titleId == "010015100B514000" || titleId == "0100ECD018EBE000" || titleId == "01009D901BC56000" || titleId == "010086A022444000" {
                // Super Mario Wonder, Paper Mario TTYD, DKC Returns HD, BALL x PIT
                minRamGB = 8
            } else {
                minRamGB = 16
            }
        } else {
            minRamGB = 8
        }
        
        let mac: String
        let ios: String
        switch minRamGB {
        case 4:
            mac = "Mac: 4GB+ RAM"
            ios = "iOS: 4GB RAM"
            if storage == "HD: ~6 GB" { storage = "HD: ~2-4 GB" }
        case 6:
            mac = "Mac: 8GB RAM"
            ios = "iOS: 6GB RAM"
        case 8:
            mac = "Mac: 8GB+ RAM"
            ios = "iOS: 6-8GB RAM"
            if storage == "HD: ~6 GB" { storage = "HD: ~8-12 GB" }
        default: // 16
            mac = "Mac: 16GB RAM"
            ios = "iOS: 8GB RAM (A17+)"
            if storage == "HD: ~6 GB" { storage = "HD: ~12-16 GB" }
        }
        
        return (mac: mac, ios: ios, storage: storage, minRamGB: minRamGB)
    }
}
