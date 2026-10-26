import Foundation

public final class GameScanner {
    public static let shared = GameScanner()
    
    private init() {}
    
    public func scan(directory: URL) -> [GameFileItem] {
        let fm = FileManager.default
        let allowedExtensions = Set(["nsp", "nsz", "xci", "xcz"])
        
        guard let enumerator = fm.enumerator(
            at: directory,
            includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey],
            options: [.skipsHiddenFiles, .skipsPackageDescendants]
        ) else {
            return []
        }
        
        var results: [GameFileItem] = []
        let titleIdRegex = try? NSRegularExpression(pattern: "\\[([0-9A-Fa-f]{16})\\]", options: [])
        let versionRegex = try? NSRegularExpression(pattern: "\\[v([0-9]+)\\]|v([0-9]+\\.[0-9]+(?:\\.[0-9]+)?)", options: [.caseInsensitive])
        
        for case let fileURL as URL in enumerator {
            let ext = fileURL.pathExtension.lowercased()
            guard allowedExtensions.contains(ext) else { continue }
            
            let resourceValues = try? fileURL.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard resourceValues?.isRegularFile == true else { continue }
            let size = Int64(resourceValues?.fileSize ?? 0)
            let filename = fileURL.lastPathComponent
            
            // Extrai Title ID
            var titleId: String? = nil
            if let regex = titleIdRegex,
               let match = regex.firstMatch(in: filename, range: NSRange(filename.startIndex..., in: filename)),
               let range = Range(match.range(at: 1), in: filename) {
                titleId = String(filename[range]).uppercased()
            } else {
                let clean = cleanGameTitle(filename)
                if let matchedTid = CompatibilityService.shared.findTitleId(forGameTitle: clean) {
                    titleId = matchedTid.uppercased()
                }
            }
            
            // Extract Version
            var versionString: String? = nil
            if let regex = versionRegex,
               let match = regex.firstMatch(in: filename, range: NSRange(filename.startIndex..., in: filename)) {
                if match.range(at: 1).location != NSNotFound, let r = Range(match.range(at: 1), in: filename) {
                    versionString = "v" + String(filename[r])
                } else if match.range(at: 2).location != NSNotFound, let r = Range(match.range(at: 2), in: filename) {
                    versionString = "v" + String(filename[r])
                }
            }
            
            // Determina tipo
            let lower = filename.lowercased()
            let type: GameFileType
            if let tid = titleId {
                if tid.hasSuffix("000") {
                    type = .base
                } else if tid.hasSuffix("800") {
                    type = .update
                } else {
                    type = .dlc
                }
            } else if lower.contains("update") || (versionString != nil && versionString != "v0") {
                type = .update
            } else if lower.contains("dlc") {
                type = .dlc
            } else if lower.contains("base") || versionString == "v0" {
                type = .base
            } else {
                type = .unknown
            }
            
            results.append(GameFileItem(
                url: fileURL,
                name: filename,
                sizeBytes: size,
                type: type,
                titleId: titleId,
                versionString: versionString
            ))
        }
        
        // Sort: Base first, then Update, then DLC
        return results.sorted { a, b in
            let scoreA: Int
            switch a.type { case .base: scoreA = 0; case .update: scoreA = 1; case .dlc: scoreA = 2; case .unknown: scoreA = 3 }
            let scoreB: Int
            switch b.type { case .base: scoreB = 0; case .update: scoreB = 1; case .dlc: scoreB = 2; case .unknown: scoreB = 3 }
            if scoreA != scoreB { return scoreA < scoreB }
            return a.name.localizedStandardCompare(b.name) == .orderedAscending
        }
    }
    
    public func groupFilesIntoGames(files: [GameFileItem]) -> [SelectableGame] {
        var groups: [String: (title: String, tid: String?, items: [GameFileItem])] = [:]
        
        for file in files {
            let clean = cleanGameTitle(file.name)
            let key: String
            if let tid = file.titleId {
                if tid.hasSuffix("000") {
                    key = tid
                } else if tid.hasSuffix("800") {
                    key = String(tid.dropLast(3)) + "000"
                } else if tid.count == 16 {
                    key = String(tid.prefix(12))
                } else {
                    key = clean.lowercased()
                }
            } else {
                key = clean.lowercased()
            }
            
            if var existing = groups[key] {
                existing.items.append(file)
                if existing.tid == nil && file.titleId != nil {
                    existing.tid = file.titleId
                }
                groups[key] = existing
            } else {
                groups[key] = (title: clean, tid: file.titleId, items: [file])
            }
        }
        
        var games: [SelectableGame] = []
        for (k, val) in groups {
            let sortedItems = val.items.sorted { a, b in
                let scoreA = (a.type == .base ? 0 : (a.type == .update ? 1 : 2))
                let scoreB = (b.type == .base ? 0 : (b.type == .update ? 1 : 2))
                return scoreA < scoreB
            }
            
            let gameTitle = val.title.isEmpty ? (sortedItems.first?.name ?? "Unknown Game") : val.title
            games.append(SelectableGame(
                id: k,
                name: gameTitle,
                titleId: val.tid,
                files: sortedItems
            ))
        }
        
        return games.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }
    
    public func cleanGameTitle(_ filename: String) -> String {
        var name = (filename as NSString).deletingPathExtension
        name = name.replacingOccurrences(of: "\\[[^\\]]+\\]", with: "", options: .regularExpression)
        name = name.replacingOccurrences(of: "\\([^\\)]+\\)", with: "", options: .regularExpression)
        name = name.replacingOccurrences(of: "(?i)v[0-9]+(?:\\.[0-9]+)*", with: "", options: .regularExpression)
        name = name.replacingOccurrences(of: "(?i)\\b(update|dlc|base)\\b", with: "", options: .regularExpression)
        name = name.replacingOccurrences(of: "[_\\-+]+", with: " ", options: .regularExpression)
        return name.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
