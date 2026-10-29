import Foundation
import AppKit

public final class LibraryScanner {
    public static let shared = LibraryScanner()
    
    private var ipaMetadataCache: [String: (modDate: Date, size: Int64, game: PortedGame)] = [:]
    private let cacheLock = NSLock()
    
    private init() {}
    
    public func scan(directories: [URL]) -> [PortedGame] {
        let fm = FileManager.default
        var games: [PortedGame] = []
        var seenURLs = Set<URL>()
        
        let home = NSHomeDirectory()
        let forbiddenPrefixes = [
            "/",
            home,
            home + "/Music",
            home + "/Pictures",
            home + "/Documents",
            home + "/Library"
        ]
        
        for dir in directories {
            let path = dir.standardized.path
            
            // STRICT PRIVACY GUARD: Never scan filesystem root or user home folders directly
            if path == "/" || path == home || path.isEmpty {
                continue
            }
            
            // Don't scan forbidden media directories
            if forbiddenPrefixes.contains(where: { $0 == path && path != "/" }) {
                continue
            }
            
            guard fm.fileExists(atPath: path) else { continue }
            
            guard let enumerator = fm.enumerator(
                at: dir,
                includingPropertiesForKeys: [.isDirectoryKey, .contentModificationDateKey, .fileSizeKey],
                options: [.skipsHiddenFiles, .skipsPackageDescendants]
            ) else { continue }
            
            for case let itemURL as URL in enumerator {
                let ext = itemURL.pathExtension.lowercased()
                
                if ext == "app" {
                    enumerator.skipDescendants()
                    if !seenURLs.contains(itemURL) {
                        seenURLs.insert(itemURL)
                        if let game = parseMacApp(at: itemURL) {
                            games.append(game)
                        }
                    }
                } else if ext == "ipa" {
                    if !seenURLs.contains(itemURL) {
                        seenURLs.insert(itemURL)
                        if let game = parseIPA(at: itemURL) {
                            games.append(game)
                        }
                    }
                }
            }
        }
        
        return games.sorted { $0.modificationDate > $1.modificationDate }
    }
    
    private var sizeCache: [URL: Int64] = [:]

    private func parseMacApp(at url: URL) -> PortedGame? {
        let fm = FileManager.default
        let plistURL = url.appendingPathComponent("Contents/Info.plist")
        guard fm.fileExists(atPath: plistURL.path) else { return nil }
        
        // Exclude the builder app itself
        if url.lastPathComponent == "NXNative.app" || url.lastPathComponent == "NxNative_Launcher.app" { return nil }
        
        var name = url.deletingPathExtension().lastPathComponent
        var bundleId: String? = nil
        var titleId: String? = nil
        var version: String? = nil
        
        if let data = try? Data(contentsOf: plistURL),
           let plist = (try? PropertyListSerialization.propertyList(from: data, options: [], format: nil)) as? [String: Any] {
            if let dispName = plist["CFBundleDisplayName"] as? String, !dispName.isEmpty {
                name = dispName
            } else if let bundleName = plist["CFBundleName"] as? String, !bundleName.isEmpty {
                name = bundleName
            }
            bundleId = plist["CFBundleIdentifier"] as? String
            titleId = plist["DMTitleID"] as? String
            version = plist["CFBundleShortVersionString"] as? String
        }
        
        // Secondary titleId extraction if missing from DMTitleID
        if titleId == nil || titleId?.isEmpty == true {
            if let bid = bundleId {
                let range = NSRange(bid.startIndex..., in: bid)
                if let regex = try? NSRegularExpression(pattern: "(0100[0-9A-Fa-f]{12})", options: [.caseInsensitive]),
                   let match = regex.firstMatch(in: bid, range: range),
                   let r = Range(match.range(at: 1), in: bid) {
                    titleId = String(bid[r]).uppercased()
                }
            }
        }
        if titleId == nil || titleId?.isEmpty == true {
            let fname = url.lastPathComponent
            let range = NSRange(fname.startIndex..., in: fname)
            if let regex = try? NSRegularExpression(pattern: "\\[(0100[0-9A-Fa-f]{12})\\]", options: [.caseInsensitive]),
               let match = regex.firstMatch(in: fname, range: range),
               let r = Range(match.range(at: 1), in: fname) {
                titleId = String(fname[r]).uppercased()
            }
        }
        if titleId == nil || titleId?.isEmpty == true {
            titleId = CompatibilityService.shared.findTitleId(forGameTitle: name)?.uppercased()
        }
        
        // Fast instant size retrieval without blocking RomFS traversal
        var size: Int64 = sizeCache[url] ?? 0
        if size == 0 {
            let romfsImg = url.appendingPathComponent("Contents/Resources/romfs.img")
            if let imgSize = (try? romfsImg.resourceValues(forKeys: [.fileSizeKey]))?.fileSize {
                size = Int64(imgSize)
            } else if let exeSize = (try? url.appendingPathComponent("Contents/MacOS/dmhost").resourceValues(forKeys: [.fileSizeKey]))?.fileSize {
                size = Int64(exeSize)
            }
        }
        
        let modDate = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate ?? Date()
        
        return PortedGame(
            name: name,
            url: url,
            platform: .macApp,
            sizeBytes: size,
            modificationDate: modDate,
            bundleId: bundleId,
            titleId: titleId?.uppercased(),
            versionString: version
        )
    }
    
    private func parseIPA(at url: URL) -> PortedGame? {
        let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
        let size = Int64(values?.fileSize ?? 0)
        let modDate = values?.contentModificationDate ?? Date()
        let pathKey = url.standardized.path
        
        cacheLock.lock()
        if let cached = ipaMetadataCache[pathKey], cached.modDate == modDate, cached.size == size {
            cacheLock.unlock()
            return cached.game
        }
        cacheLock.unlock()

        var name = url.deletingPathExtension().lastPathComponent
        var bundleId: String? = nil
        var titleId: String? = nil
        var version: String? = nil
        
        // Fast inspect Payload/*/Info.plist from inside the IPA (zip archive)
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
        process.arguments = ["-p", url.path, "Payload/*/Info.plist"]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()
        if (try? process.run()) != nil {
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            if !data.isEmpty,
               let plist = (try? PropertyListSerialization.propertyList(from: data, options: [], format: nil)) as? [String: Any] {
                if let dispName = plist["CFBundleDisplayName"] as? String, !dispName.isEmpty {
                    name = dispName
                } else if let bundleName = plist["CFBundleName"] as? String, !bundleName.isEmpty && bundleName != "DmIOS" {
                    name = bundleName
                }
                bundleId = plist["CFBundleIdentifier"] as? String
                titleId = plist["DMTitleID"] as? String
                version = plist["CFBundleShortVersionString"] as? String
            }
        }
        
        // Extract TitleID from CFBundleIdentifier if needed
        if titleId == nil || titleId?.isEmpty == true {
            if let bid = bundleId {
                let range = NSRange(bid.startIndex..., in: bid)
                if let regex = try? NSRegularExpression(pattern: "(0100[0-9A-Fa-f]{12})", options: [.caseInsensitive]),
                   let match = regex.firstMatch(in: bid, range: range),
                   let r = Range(match.range(at: 1), in: bid) {
                    titleId = String(bid[r]).uppercased()
                }
            }
        }
        
        // Extract TitleID from filename regex [0100...]
        if titleId == nil || titleId?.isEmpty == true {
            let fname = url.lastPathComponent
            let range = NSRange(fname.startIndex..., in: fname)
            if let regex = try? NSRegularExpression(pattern: "\\[(0100[0-9A-Fa-f]{12})\\]", options: [.caseInsensitive]),
               let match = regex.firstMatch(in: fname, range: range),
               let r = Range(match.range(at: 1), in: fname) {
                titleId = String(fname[r]).uppercased()
            }
        }
        
        // Fall back to title database matching across 29,000+ Switch titles
        if titleId == nil || titleId?.isEmpty == true {
            titleId = CompatibilityService.shared.findTitleId(forGameTitle: name)?.uppercased()
        }
        
        if bundleId == nil, let tid = titleId {
            bundleId = "org.nxnative.\(tid.lowercased())"
        }
        
        let game = PortedGame(
            name: name,
            url: url,
            platform: .ios,
            sizeBytes: size,
            modificationDate: modDate,
            bundleId: bundleId,
            titleId: titleId?.uppercased(),
            versionString: version
        )
        cacheLock.lock()
        ipaMetadataCache[pathKey] = (modDate: modDate, size: size, game: game)
        cacheLock.unlock()
        return game
    }
    
    private func calculateDirectorySize(url: URL) -> Int64 {
        let fm = FileManager.default
        guard let files = fm.enumerator(at: url, includingPropertiesForKeys: [.fileSizeKey]) else { return 0 }
        var total: Int64 = 0
        for case let file as URL in files {
            if let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize {
                total += Int64(size)
            }
        }
        return total
    }
    
    public func launchGame(at url: URL, env: [String: String]? = nil, completion: @escaping (Bool, String?) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            // Strip macOS quarantine attribute to bypass Gatekeeper unidentified developer blocks
            let cleanProc = Process()
            cleanProc.executableURL = URL(fileURLWithPath: "/usr/bin/xattr")
            cleanProc.arguments = ["-cr", url.path]
            try? cleanProc.run()
            cleanProc.waitUntilExit()
            
            DispatchQueue.main.async {
                let config = NSWorkspace.OpenConfiguration()
                config.activates = true
                if let envDict = env {
                    config.environment = envDict
                }
                
                NSWorkspace.shared.openApplication(at: url, configuration: config) { app, error in
                    if let err = error {
                        completion(false, err.localizedDescription)
                    } else {
                        GameControllerService.shared.activeRunningGameApp = app
                        completion(true, nil)
                    }
                }
            }
        }
    }
}
