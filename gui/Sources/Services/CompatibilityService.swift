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
            
            let specs = self.resolveSpecs(titleId: tid, readmeRelPath: path)
            
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
                    storageReq: specs.storage
                )
            )
        }
        
        self.cachedGames = items
        return items
    }
    
    public func findTitleId(forGameTitle cleanTitle: String) -> String? {
        let normalized = cleanTitle.lowercased().filter { $0.isLetter || $0.isNumber }
        guard !normalized.isEmpty else { return nil }
        
        var list = cachedGames
        if list.isEmpty {
            let localPath = "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/dm/games/README.md"
            if let text = try? String(contentsOfFile: localPath, encoding: .utf8) {
                list = parseMarkdownTable(text)
            }
        }
        
        for game in list {
            let normGame = game.title.lowercased().filter { $0.isLetter || $0.isNumber }
            if normGame == normalized || (normGame.contains(normalized) && normalized.count >= 6) || (normalized.contains(normGame) && normGame.count >= 6) {
                return game.titleId
            }
        }
        return nil
    }

    /// Resolves minimum Mac and iOS hardware specs (RAM, Storage) from nxnative game profiles and README notes.
    private func resolveSpecs(titleId: String, readmeRelPath: String) -> (mac: String, ios: String, storage: String) {
        let baseDir = "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/dm/games/\(titleId)"
        let tsvPath = "\(baseDir)/game.tsv"
        let readmePath = "\(baseDir)/README.md"
        
        var memReserve = ""
        if let tsvContent = try? String(contentsOfFile: tsvPath, encoding: .utf8) {
            for line in tsvContent.components(separatedBy: .newlines) {
                let parts = line.components(separatedBy: "\t")
                if parts.count >= 2 && parts[0].trimmingCharacters(in: .whitespaces) == "mem_reserve" {
                    memReserve = parts[1].trimmingCharacters(in: .whitespaces)
                    break
                }
            }
        }
        
        var storage = "HD: ~6 GB"
        if let rmContent = try? String(contentsOfFile: readmePath, encoding: .utf8) {
            let pattern = #"(?:IPA|RomFS|app|game)[^\n\r]{0,35}?(?:at about|is about|about|is)?\s*([0-9]+(?:\.[0-9]+)?)\s*GB"#
            if let regex = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) {
                let ns = rmContent as NSString
                if let match = regex.firstMatch(in: rmContent, options: [], range: NSRange(location: 0, length: ns.length)), match.numberOfRanges >= 2 {
                    let gbVal = ns.substring(with: match.range(at: 1))
                    storage = "HD: ~\(gbVal) GB"
                }
            }
        }
        
        let mac: String
        let ios: String
        if memReserve.contains("0x180000000") || memReserve.contains("0x140000000") {
            mac = "Mac: 16GB+ RAM"
            ios = "iOS: 8GB RAM (A17+)"
            if storage == "HD: ~6 GB" { storage = "HD: ~12-16 GB" }
        } else if memReserve.contains("0x100000000") {
            mac = "Mac: 8-16GB RAM"
            ios = "iOS: 6-8GB RAM (A15+)"
            if storage == "HD: ~6 GB" { storage = "HD: ~8-16 GB" }
        } else if memReserve.contains("0x80000000") || memReserve.contains("0xC0000000") {
            mac = "Mac: 8GB RAM"
            ios = "iOS: 4-6GB RAM (A14+)"
            if storage == "HD: ~6 GB" { storage = "HD: ~2-4 GB" }
        } else {
            mac = "Mac: 8GB+ RAM"
            ios = "iOS: 6GB+ RAM"
        }
        
        return (mac: mac, ios: ios, storage: storage)
    }
}
