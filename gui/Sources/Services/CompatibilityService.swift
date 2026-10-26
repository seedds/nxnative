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
            
            items.append(
                CompatibleGameItem(
                    title: title,
                    titleId: tid,
                    versionTested: ver,
                    sdk: sdk,
                    engine: engine,
                    graphicsApi: api,
                    readmeRelPath: path
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
}
