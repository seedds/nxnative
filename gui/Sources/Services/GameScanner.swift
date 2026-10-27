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
            
            // Extrai Title ID (1. Tag [0100...] no nome, 2. Header PFS0 .tik, 3. Busca por nome no banco)
            var titleId: String? = nil
            var hasExplicitTitleId = false
            if let regex = titleIdRegex,
               let match = regex.firstMatch(in: filename, range: NSRange(filename.startIndex..., in: filename)),
               let range = Range(match.range(at: 1), in: filename) {
                titleId = String(filename[range]).uppercased()
                hasExplicitTitleId = true
            } else if let headerTid = extractTitleIdFromPFS0(url: fileURL) {
                titleId = headerTid.uppercased()
                hasExplicitTitleId = true
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
            
            // Determina tipo (Base, Update ou DLC)
            let lower = filename.lowercased()
            let parentFolderLower = fileURL.deletingLastPathComponent().lastPathComponent.lowercased()
            let type: GameFileType
            if hasExplicitTitleId, let tid = titleId, let val = UInt64(tid, radix: 16) {
                let low = val & 0xFFF
                if low == 0 {
                    type = .base
                } else if low == 0x800 {
                    type = .update
                } else {
                    type = .dlc
                }
            } else if lower.contains("dlc") || parentFolderLower == "dlc" || parentFolderLower == "dlcs" {
                type = .dlc
            } else if lower.contains("update") || lower.contains("patch") || parentFolderLower == "update" || parentFolderLower == "updates" || (versionString != nil && versionString != "v0" && !lower.contains("base")) {
                type = .update
            } else if let tid = titleId {
                if tid.hasSuffix("000") {
                    type = .base
                } else if tid.hasSuffix("800") {
                    type = .update
                } else {
                    type = .dlc
                }
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
    
    /// Converte qualquer Title ID (Base ...000, Update ...800, ou DLC ...(Y+1)ZZZ) para o Title ID de 16 caracteres do jogo Base.
    public func baseApplicationTitleId(for tid: String) -> String? {
        let trimmed = tid.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        guard trimmed.count == 16, let val = UInt64(trimmed, radix: 16) else { return nil }
        let low = val & 0xFFF
        let baseVal: UInt64
        if low == 0 {
            baseVal = val
        } else if low == 0x800 {
            baseVal = val & ~0xFFF
        } else {
            baseVal = (val & ~0xFFF) &- 0x1000
        }
        return String(format: "%016llX", baseVal)
    }
    
    /// Lê o cabeçalho PFS0 de um arquivo .nsp / .nsz para extrair o Title ID do ticket (<32-hex>.tik), igual ao build_ipa_from_nsp.py.
    public func extractTitleIdFromPFS0(url: URL) -> String? {
        let ext = url.pathExtension.lowercased()
        guard ext == "nsp" || ext == "nsz" else { return nil }
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        
        guard let header = try? handle.read(upToCount: 16), header.count == 16 else { return nil }
        guard header[0] == 0x50, header[1] == 0x46, header[2] == 0x53, header[3] == 0x30 else { return nil } // "PFS0"
        
        let numFiles = header.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self).littleEndian }
        let strTableSize = header.withUnsafeBytes { $0.load(fromByteOffset: 8, as: UInt32.self).littleEndian }
        guard numFiles > 0, numFiles < 2048, strTableSize > 0, strTableSize < 262144 else { return nil }
        
        let entriesBytes = Int(numFiles) * 24
        guard let rest = try? handle.read(upToCount: entriesBytes + Int(strTableSize)),
              rest.count >= entriesBytes + Int(strTableSize) else { return nil }
        
        let strTableData = rest.subdata(in: entriesBytes..<(entriesBytes + Int(strTableSize)))
        guard let rawStrings = String(data: strTableData, encoding: .ascii) else { return nil }
        
        let tikRegex = try? NSRegularExpression(pattern: "([0-9A-Fa-f]{16})[0-9A-Fa-f]{16}\\.tik", options: [])
        if let regex = tikRegex,
           let match = regex.firstMatch(in: rawStrings, range: NSRange(rawStrings.startIndex..., in: rawStrings)),
           let range = Range(match.range(at: 1), in: rawStrings) {
            return String(rawStrings[range]).uppercased()
        }
        return nil
    }
    
    public func groupFilesIntoGames(files: [GameFileItem]) -> [SelectableGame] {
        var groups: [String: (title: String, tid: String?, items: [GameFileItem])] = [:]
        var looseAddons: [GameFileItem] = []
        
        for file in files {
            let clean = cleanGameTitle(file.name)
            let baseTid = file.titleId.flatMap { baseApplicationTitleId(for: $0) }
            
            // Se for um Update ou DLC sem Title ID explícito, guarda para anexar ao jogo base da mesma pasta
            if baseTid == nil && (file.type == .update || file.type == .dlc) {
                looseAddons.append(file)
                continue
            }
            
            let key: String
            if let bTid = baseTid {
                key = bTid
            } else {
                key = clean.lowercased()
            }
            
            if var existing = groups[key] {
                existing.items.append(file)
                if existing.tid == nil, let bTid = baseTid {
                    existing.tid = bTid
                }
                if file.type == .base && !clean.isEmpty {
                    existing.title = clean
                }
                groups[key] = existing
            } else {
                groups[key] = (title: clean, tid: baseTid ?? file.titleId, items: [file])
            }
        }
        
        // Anexa Updates/DLCs sem Title ID explícito ao jogo Base correspondente (por nome ou se houver apenas 1 jogo Base na pasta)
        if !looseAddons.isEmpty {
            let groupKeys = Array(groups.keys)
            for addon in looseAddons {
                let addonClean = cleanGameTitle(addon.name).lowercased()
                if let matchedKey = groupKeys.first(where: { key in
                    guard let grp = groups[key] else { return false }
                    let grpTitle = grp.title.lowercased()
                    return !grpTitle.isEmpty && !addonClean.isEmpty && (addonClean.contains(grpTitle) || grpTitle.contains(addonClean))
                }) {
                    groups[matchedKey]?.items.append(addon)
                } else if groupKeys.count == 1, let onlyKey = groupKeys.first {
                    groups[onlyKey]?.items.append(addon)
                } else {
                    let fallbackKey = addonClean.isEmpty ? addon.name.lowercased() : addonClean
                    if var existing = groups[fallbackKey] {
                        existing.items.append(addon)
                        groups[fallbackKey] = existing
                    } else {
                        groups[fallbackKey] = (title: cleanGameTitle(addon.name), tid: addon.titleId, items: [addon])
                    }
                }
            }
        }
        
        // Caso ainda existam grupos apenas com Updates/DLCs e apenas 1 grupo com jogo Base na mesma pasta, mescla automaticamente
        let baseGroupKeys = groups.keys.filter { key in
            groups[key]?.items.contains(where: { $0.type == .base || $0.type == .unknown }) == true
        }
        let addonOnlyKeys = groups.keys.filter { key in
            !baseGroupKeys.contains(key)
        }
        if baseGroupKeys.count == 1, let mainKey = baseGroupKeys.first, !addonOnlyKeys.isEmpty {
            for addonKey in addonOnlyKeys {
                if let addonGroup = groups.removeValue(forKey: addonKey) {
                    groups[mainKey]?.items.append(contentsOf: addonGroup.items)
                }
            }
        }
        
        var games: [SelectableGame] = []
        for (k, val) in groups {
            let sortedItems = val.items.sorted { a, b in
                let scoreA = (a.type == .base ? 0 : (a.type == .update ? 1 : 2))
                let scoreB = (b.type == .base ? 0 : (b.type == .update ? 1 : 2))
                if scoreA != scoreB { return scoreA < scoreB }
                return a.name.localizedStandardCompare(b.name) == .orderedAscending
            }
            
            let baseItemTitle = sortedItems.first(where: { $0.type == .base }).map { cleanGameTitle($0.name) }
            let resolvedTitle = (baseItemTitle?.isEmpty == false ? baseItemTitle! : val.title)
            let gameTitle = resolvedTitle.isEmpty ? (sortedItems.first?.name ?? "Unknown Game") : resolvedTitle
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
