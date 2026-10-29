import Foundation

public final class CompatibilityService {
    public static let shared = CompatibilityService()
    
    private let githubRawURL = URL(string: "https://raw.githubusercontent.com/seedds/nxnative/main/dm/games/README.md")!
    public private(set) var cachedGames: [CompatibleGameItem] = []
    
    // Switch Titles Master Database (loaded only lazily on-demand, evicted automatically after 15s)
    private var titlesDatabase: [(tid: String, name: String, norm: String, words: Set<String>)] = []
    private var normalizedExactToTid: [String: String] = [:]
    private var resolvedTitleIdCache: [String: String] = [:]
    private var titlesIndexLoaded: Bool = false
    private let titlesIndexLock = NSLock()
    private var databaseEvictionWorkItem: DispatchWorkItem?
    
    private init() {
        // Do not eagerly load 29,000 titles into RAM on startup; load lazily on-demand only
    }
    
    public func purgeTransientCaches() {
        titlesIndexLock.lock()
        databaseEvictionWorkItem?.cancel()
        databaseEvictionWorkItem = nil
        titlesDatabase.removeAll(keepingCapacity: false)
        normalizedExactToTid.removeAll(keepingCapacity: false)
        titlesIndexLoaded = false
        titlesIndexLock.unlock()
    }
    
    private func scheduleDatabaseEviction() {
        databaseEvictionWorkItem?.cancel()
        let item = DispatchWorkItem { [weak self] in
            self?.purgeTransientCaches()
        }
        databaseEvictionWorkItem = item
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 15.0, execute: item)
    }
    
    public func loadTitlesDatabaseIfNeeded() {
        titlesIndexLock.lock()
        defer { titlesIndexLock.unlock() }
        guard !titlesIndexLoaded else { return }
        
        let fm = FileManager.default
        let candidates = [
            URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images/switch_titles_artwork.json"),
            fm.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/nxnative/switch_titles_artwork.json"),
            Bundle.main.url(forResource: "switch_titles_artwork", withExtension: "json")
        ].compactMap { $0 }
        
        for url in candidates {
            if fm.fileExists(atPath: url.path),
               let data = try? Data(contentsOf: url),
               let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: [String: Any]] {
                var db: [(tid: String, name: String, norm: String, words: Set<String>)] = []
                var exactMap: [String: String] = [:]
                db.reserveCapacity(json.count)
                exactMap.reserveCapacity(json.count)
                
                for (tid, dict) in json {
                    let upperTid = tid.uppercased()
                    guard let name = dict["name"] as? String, !name.isEmpty else { continue }
                    let words = tokenizeWords(name)
                    let norm = words.joined(separator: " ")
                    db.append((tid: upperTid, name: name, norm: norm, words: Set(words)))
                    if !norm.isEmpty && exactMap[norm] == nil {
                        exactMap[norm] = upperTid
                    }
                }
                
                self.titlesDatabase = db
                self.normalizedExactToTid = exactMap
                self.titlesIndexLoaded = true
                break
            }
        }
    }
    
    private func tokenizeWords(_ raw: String) -> [String] {
        let folded = raw.folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
        let cleaned = folded
            .replacingOccurrences(of: "\u{2122}", with: "")
            .replacingOccurrences(of: "\u{00AE}", with: "")
            .replacingOccurrences(of: "\u{00A9}", with: "")
            .replacingOccurrences(of: ":", with: " ")
            .replacingOccurrences(of: "-", with: " ")
            .replacingOccurrences(of: "–", with: " ")
            .replacingOccurrences(of: "—", with: " ")
            .replacingOccurrences(of: "_", with: " ")
            .replacingOccurrences(of: "+", with: " ")
        
        let stopWords: Set<String> = [
            "the", "of", "and", "edition", "nintendo", "switch", "2", "hd",
            "remastered", "definitive", "complete", "reimagined", "asian", "release",
            "version", "bundle", "pack", "collection"
        ]
        
        let components = cleaned.components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .map { $0.lowercased() }
        
        let filtered = components.filter { !stopWords.contains($0) }
        return filtered.isEmpty ? components : filtered
    }
    
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
    
    private static let fallbackRecentPortTimestamps: [String: TimeInterval] = [
        "0100A3900C3E2000": 1793207710, // Paper Mario: The Origami King
        "01002E7016C46000": 1793201662, // Cult of the Lamb
        "01001F5010DFA000": 1793195846, // Pokémon Legends: Arceus
        "0100965017338000": 1793195495, // Super Mario Party Jamboree
        "0100FF500E34A000": 1793183436, // Xenoblade Chronicles: Definitive Edition
        "0100B8A026F14000": 1793180603, // FINAL FANTASY RESONANCE (Asian)
        "0100606026F12000": 1793180600, // FINAL FANTASY RESONANCE
        "010062B01525C000": 1793176619, // Persona 4 Golden
        "0100B8E016F76000": 1793158157, // NieR:Automata (Asian)
        "010056B015FE8000": 1793158150, // NieR:Automata
        "010067300059A000": 1793147162, // Mario + Rabbids Kingdom Battle
        "0100BC300CB48000": 1793142784, // FINAL FANTASY X/X-2 HD Remaster
        "0100D5B026844000": 1793128039, // Silver Pines
        "0100698009C6E000": 1793127142, // Blasphemous
        "010040801A4C8000": 1793123718, // Blasphemous 2
        "010032E00E6E2000": 1793123433, // Return of the Obra Dinn
        "0100E95004038000": 1793102788, // Xenoblade Chronicles 2
        "010045C014650000": 1793098225, // 13 Sentinels (JP)
        "0100CE4010AAC000": 1793097474, // FINAL FANTASY CRYSTAL CHRONICLES
        "0100A3D008C5C000": 1793085542, // Pokémon Scarlet
        "01006000040C2000": 1793070812, // Yoshi's Crafted World
        "010074F013262000": 1793059085, // Xenoblade Chronicles 3
        "0100317013770000": 1793056426, // Mario + Rabbids Sparks of Hope
        "01004D300C5AE000": 1793040665, // Kirby and the Forgotten Land
        "01009D901BC56000": 1793031222  // Donkey Kong Country Returns HD
    ]

    private func loadAddedTimestamps() -> [String: TimeInterval] {
        var result = Self.fallbackRecentPortTimestamps
        let repoPath = "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative"
        guard FileManager.default.fileExists(atPath: repoPath) else {
            return result
        }

        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/git")
        proc.arguments = [
            "-C", repoPath,
            "log",
            "--diff-filter=A",
            "--name-only",
            "--format=TS:%ct",
            "--",
            "dm/games/*/game.tsv"
        ]
        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = FileHandle.nullDevice

        do {
            try proc.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            proc.waitUntilExit()
            if let output = String(data: data, encoding: .utf8) {
                var currentTimestamp: TimeInterval = 0
                for rawLine in output.components(separatedBy: .newlines) {
                    let line = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
                    if line.hasPrefix("TS:") {
                        if let ts = TimeInterval(line.dropFirst(3)) {
                            currentTimestamp = ts
                        }
                    } else if line.hasPrefix("dm/games/") && line.hasSuffix("/game.tsv") && currentTimestamp > 0 {
                        let parts = line.components(separatedBy: "/")
                        if parts.count >= 3 {
                            let tid = parts[2].uppercased()
                            if result[tid] == nil || currentTimestamp > (result[tid] ?? 0) {
                                result[tid] = currentTimestamp
                            }
                        }
                    }
                }
            }
        } catch {
            // Fall back to static recent timestamps if git invocation fails
        }
        return result
    }

    public func parseMarkdownTable(_ markdown: String) -> [CompatibleGameItem] {
        var items: [CompatibleGameItem] = []
        let pattern = #"^\|\s*\[([^\]]+)\]\(([^)]+)\)\s*\|\s*`?([0-9A-Fa-f]{16})`?\s*\|\s*([^|]+)\s*\|\s*([^|]+)\s*\|\s*([^|]+)\s*\|\s*([^|]+)\s*\|"#
        
        guard let regex = try? NSRegularExpression(pattern: pattern, options: [.anchorsMatchLines]) else {
            return []
        }
        
        let addedTimestamps = loadAddedTimestamps()
        let sortedTimestamps = addedTimestamps.values.sorted(by: >)
        let newThreshold: TimeInterval = sortedTimestamps.count >= 15
            ? sortedTimestamps[14]
            : (sortedTimestamps.last ?? 1)
        
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
            let upperTid = tid.uppercased()
            let addedTs = addedTimestamps[upperTid] ?? 0
            let isNewGame = addedTs > 0 && addedTs >= newThreshold
            
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
                    minRamGB: specs.minRamGB,
                    minChipTier: specs.minChipTier,
                    isNew: isNewGame,
                    addedTimestamp: addedTs
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
        titlesIndexLock.lock()
        if let cached = resolvedTitleIdCache[cleanTitle] {
            titlesIndexLock.unlock()
            return cached
        }
        titlesIndexLock.unlock()

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
        
        // 1. Exact folded match against profile games
        for game in list {
            let normGame = game.title
                .folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
                .filter { $0.isLetter || $0.isNumber }
            if normGame == rawNorm {
                let tid = game.titleId.uppercased()
                titlesIndexLock.lock()
                resolvedTitleIdCache[cleanTitle] = tid
                titlesIndexLock.unlock()
                return tid
            }
        }
        
        // 2. Semantic word match against profile games
        for game in list {
            let semGame = normalizeTitle(game.title)
            if !semGame.isEmpty && (semGame == semanticNorm || (semGame.contains(semanticNorm) && semanticNorm.count >= 5) || (semanticNorm.contains(semGame) && semGame.count >= 5)) {
                let tid = game.titleId.uppercased()
                titlesIndexLock.lock()
                resolvedTitleIdCache[cleanTitle] = tid
                titlesIndexLock.unlock()
                return tid
            }
        }
        
        // 3. Search 29,000+ official Nintendo Switch title database (switch_titles_artwork.json)
        loadTitlesDatabaseIfNeeded()
        defer {
            scheduleDatabaseEviction()
        }
        
        let qWords = tokenizeWords(cleanTitle)
        let qNorm = qWords.joined(separator: " ")
        if let exactMatch = normalizedExactToTid[qNorm] {
            titlesIndexLock.lock()
            resolvedTitleIdCache[cleanTitle] = exactMatch
            titlesIndexLock.unlock()
            return exactMatch
        }
        
        let qWordSet = Set(qWords)
        if !qWordSet.isEmpty {
            var bestTid: String? = nil
            var bestScore: Double = 0.0
            
            for item in titlesDatabase {
                if item.words == qWordSet {
                    titlesIndexLock.lock()
                    resolvedTitleIdCache[cleanTitle] = item.tid
                    titlesIndexLock.unlock()
                    return item.tid
                }
                
                if qWordSet.isSubset(of: item.words) {
                    let score = Double(qWordSet.count) / Double(item.words.count)
                    if score > bestScore {
                        bestScore = score
                        bestTid = item.tid
                        if score >= 0.99 { break }
                    }
                } else if item.words.isSubset(of: qWordSet) && item.words.count >= 2 {
                    let score = Double(item.words.count) / Double(qWordSet.count)
                    if score > bestScore {
                        bestScore = score
                        bestTid = item.tid
                    }
                }
            }
            
            if let matched = bestTid, bestScore >= 0.5 {
                titlesIndexLock.lock()
                resolvedTitleIdCache[cleanTitle] = matched
                titlesIndexLock.unlock()
                return matched
            }
        }
        
        // 4. Substring match on folded characters from profiles
        for game in list {
            let normGame = game.title
                .folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
                .filter { $0.isLetter || $0.isNumber }
            if (normGame.contains(rawNorm) && rawNorm.count >= 5) || (rawNorm.contains(normGame) && normGame.count >= 5) {
                let tid = game.titleId.uppercased()
                titlesIndexLock.lock()
                resolvedTitleIdCache[cleanTitle] = tid
                titlesIndexLock.unlock()
                return tid
            }
        }
        
        return nil
    }

    /// Resolves minimum Mac and iOS hardware specs (RAM, Storage, minRamGB tier: 4, 6, 8, or 16, and iPhone chip tier) from nxnative game profiles and README notes.
    private func resolveSpecs(titleId: String, readmeRelPath: String, engine: String = "") -> (mac: String, ios: String, storage: String, minRamGB: Int, minChipTier: IphoneChipTier) {
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
        
        let upperTid = titleId.uppercased()
        let a18OptimizedTitles: Set<String> = [
            "0100FF500E34A000", // Xenoblade Chronicles: Definitive Edition
            "0100E95004038000", // Xenoblade Chronicles 2
            "0100B04011742000", // Monster Hunter Rise
            "0100965017338000", // Super Mario Party Jamboree
            "01001F5010DFA000", // Pokémon Legends: Arceus
            "0100A3D008C5C000", // Pokémon Scarlet
            "01006A800016E000", // Super Smash Bros. Ultimate
            "01004A4010FEA000", // Bayonetta 3
            "01002B00111A2000", // Hyrule Warriors: Age of Calamity
            "0100A3501946E000", // OCTOPATH TRAVELER II
            "01006C300E9F0000"  // DRAGON QUEST XI S
        ]
        
        let mac: String
        let ios: String
        let minChipTier: IphoneChipTier
        switch minRamGB {
        case 4:
            mac = "Mac: 4GB+ RAM"
            ios = "iPhone: A15+ (4GB)"
            minChipTier = .a15
            if storage == "HD: ~6 GB" { storage = "HD: ~2-4 GB" }
        case 6:
            mac = "Mac: 8GB RAM"
            ios = "iPhone: A16+ (6GB)"
            minChipTier = .a16
        case 8:
            mac = "Mac: 8GB+ RAM"
            if isUnreal || a18OptimizedTitles.contains(upperTid) || (maxResidentGB ?? 0) >= 2.75 {
                ios = "iPhone: A18+ (8GB)"
                minChipTier = .a18
            } else {
                ios = "iPhone: A17 Pro+ (8GB)"
                minChipTier = .a17Pro
            }
            if storage == "HD: ~6 GB" { storage = "HD: ~8-12 GB" }
        default: // 16
            mac = "Mac: 16GB RAM"
            ios = "iPhone: A18 Pro (8GB)"
            minChipTier = .a18Pro
            if storage == "HD: ~6 GB" { storage = "HD: ~12-16 GB" }
        }
        
        return (mac: mac, ios: ios, storage: storage, minRamGB: minRamGB, minChipTier: minChipTier)
    }
}
