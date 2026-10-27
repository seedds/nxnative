import Foundation
import AppKit
import SwiftUI

public final class GameCoverService: ObservableObject {
    public static let shared = GameCoverService()

    @Published public var loadedCovers: [String: NSImage] = [:]
    private let fileManager = FileManager.default
    private let cacheDirectory: URL
    private var inFlightDownloads = Set<String>()

    private init() {
        let appSup = fileManager.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/nxnative/covers")
        try? fileManager.createDirectory(at: appSup, withIntermediateDirectories: true)
        self.cacheDirectory = appSup
    }

    public func coverImage(for game: PortedGame) -> NSImage? {
        let titleId = game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name)
        let key = titleId ?? game.uniqueKey
        
        if let memoryImage = loadedCovers[key] {
            return memoryImage
        }

        // 1. Check game bundle's Contents/Resources for cover art
        let bundleResources = game.url.appendingPathComponent("Contents/Resources")
        for filename in ["cover.jpg", "cover.png", "coverHQ.jpg", "boxart.jpg", "boxart.png"] {
            let bundleCover = bundleResources.appendingPathComponent(filename)
            if fileManager.fileExists(atPath: bundleCover.path),
               let img = NSImage(contentsOf: bundleCover) {
                loadedCovers[key] = img
                return img
            }
        }

        // 2. Check disk cache in ~/Library/Application Support/nxnative/covers/<TitleID>.jpg
        if let tid = titleId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid.uppercased()).jpg")
            if fileManager.fileExists(atPath: cached.path),
               let img = NSImage(contentsOf: cached) {
                loadedCovers[key] = img
                injectCoverIfPossible(sourceImageURL: cached, targetAppURL: game.url)
                return img
            }
        }

        // 3. Check local docs/images repository for matching slug art
        if let localCover = findLocalRepoCover(forGameName: game.name, titleId: titleId) {
            if let img = NSImage(contentsOf: localCover) {
                loadedCovers[key] = img
                if let tid = titleId, !tid.isEmpty {
                    let cacheTarget = cacheDirectory.appendingPathComponent("\(tid.uppercased()).jpg")
                    try? fileManager.copyItem(at: localCover, to: cacheTarget)
                }
                injectCoverIfPossible(sourceImageURL: localCover, targetAppURL: game.url)
                return img
            }
        }

        // 4. Initiate async background download if not yet cached
        fetchOfficialCoverAsync(titleId: titleId?.uppercased(), gameName: game.name, targetAppURL: game.url)

        // 5. Default to app icon while waiting or fallback
        return NSWorkspace.shared.icon(forFile: game.url.path)
    }

    /// Lookup cover art for Compatibility View grid cards
    public func coverImage(titleId: String?, gameName: String) -> NSImage? {
        let resolvedId = titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: gameName)
        let key = resolvedId ?? gameName
        
        if let memoryImage = loadedCovers[key] {
            return memoryImage
        }
        
        if let tid = resolvedId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid.uppercased()).jpg")
            if fileManager.fileExists(atPath: cached.path),
               let img = NSImage(contentsOf: cached) {
                loadedCovers[key] = img
                return img
            }
        }
        
        if let localCover = findLocalRepoCover(forGameName: gameName, titleId: resolvedId) {
            if let img = NSImage(contentsOf: localCover) {
                loadedCovers[key] = img
                return img
            }
        }
        
        fetchOfficialCoverAsync(titleId: resolvedId?.uppercased(), gameName: gameName, targetAppURL: nil)
        return nil
    }

    public func injectCoverIntoBundle(at appURL: URL, gameName: String, titleId: String?) {
        let resolvedId = titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: gameName)
        if let localCover = findLocalRepoCover(forGameName: gameName, titleId: resolvedId) {
            injectCoverIfPossible(sourceImageURL: localCover, targetAppURL: appURL)
        } else if let tid = resolvedId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid.uppercased()).jpg")
            if fileManager.fileExists(atPath: cached.path) {
                injectCoverIfPossible(sourceImageURL: cached, targetAppURL: appURL)
            }
        }
    }

    private func injectCoverIfPossible(sourceImageURL: URL, targetAppURL: URL) {
        guard targetAppURL.pathExtension.lowercased() == "app" else { return }
        let resources = targetAppURL.appendingPathComponent("Contents/Resources")
        let targetCover = resources.appendingPathComponent("cover.jpg")
        
        if !fileManager.fileExists(atPath: targetCover.path) && fileManager.fileExists(atPath: resources.path) {
            try? fileManager.copyItem(at: sourceImageURL, to: targetCover)
        }
    }

    private func findLocalRepoCover(forGameName name: String, titleId: String?) -> URL? {
        let searchDirectories = [
            "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images/covers",
            "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images",
            Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/images/covers").path,
            Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/images").path,
            Bundle.main.resourcePath ?? ""
        ]

        for dirPath in searchDirectories where !dirPath.isEmpty {
            let dirURL = URL(fileURLWithPath: dirPath)
            guard fileManager.fileExists(atPath: dirURL.path) else { continue }

            if let tid = titleId, !tid.isEmpty {
                for ext in ["jpg", "png", "jpeg"] {
                    let byId = dirURL.appendingPathComponent("\(tid.uppercased()).\(ext)")
                    if fileManager.fileExists(atPath: byId.path) { return byId }
                }
            }

            let slugs = generateSlugs(from: name)
            for slug in slugs {
                for ext in ["jpg", "png", "jpeg"] {
                    let candidate = dirURL.appendingPathComponent("\(slug).\(ext)")
                    if fileManager.fileExists(atPath: candidate.path) {
                        return candidate
                    }
                }
            }
        }
        return nil
    }

    private func generateSlugs(from name: String) -> [String] {
        var results = [String]()
        var base = name.lowercased()
        base = base.replacingOccurrences(of: "the legend of zelda", with: "zelda")
        base = base.replacingOccurrences(of: "legend of zelda", with: "zelda")
        base = base.replacingOccurrences(of: "’", with: "")
        base = base.replacingOccurrences(of: "'", with: "")
        base = base.replacingOccurrences(of: ":", with: "")
        base = base.replacingOccurrences(of: "+", with: "-")
        base = base.replacingOccurrences(of: "&", with: "and")
        
        let slug1 = base
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .joined(separator: "-")
        results.append(slug1)
        
        let slug2 = name.lowercased()
            .replacingOccurrences(of: "’", with: "")
            .replacingOccurrences(of: "'", with: "")
            .replacingOccurrences(of: ":", with: "")
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .joined(separator: "-")
        if slug2 != slug1 { results.append(slug2) }
        return results
    }

    private func fetchOfficialCoverAsync(titleId: String?, gameName: String, targetAppURL: URL?) {
        let searchKey = titleId ?? gameName
        guard !inFlightDownloads.contains(searchKey) else { return }
        inFlightDownloads.insert(searchKey)

        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            defer {
                DispatchQueue.main.async { self.inFlightDownloads.remove(searchKey) }
            }

            var candidates: [URL] = []
            
            // 1. Known GameTDB IDs
            let gameIdMap: [String: String] = [
                "0100F2C0115B6000": "AXN7A", "01007EF00011E000": "AAAAA",
                "01006BB00C6F0000": "AR3NA", "01002B00111A2000": "AXEAA",
                "0100A9D01C446000": "BD9VA", "01006C300E9F0000": "ALC7A",
                "010028600EBDA000": "AUZPA", "0100000000010000": "AAACA",
                "010015100B514000": "AQMXA", "0100152000022000": "AABPA",
                "01006F8002326000": "ACBAA", "010093801237C000": "AYL8A",
                "0100CD801CE5E000": "BEYKA", "01004D300C5AE000": "ARZGA",
                "0100ECD018EBE000": "A9QWA", "01005CA01580E000": "AXU7A",
                "010055D009F78000": "ANVYA", "010069401ADB8000": "AQ36A",
                "01003FC01670C000": "AU9HA", "01004A4010FEA000": "AWGKA",
                "0100535012974000": "AUY4A", "0100B04011742000": "AXSEA",
                "010056B015FE8000": "A49WA", "0100C1F0051B6000": "AFWTA",
                "0100DCA0064A6000": "AG3JA", "0100000011D90000": "AX7QA",
                "0100A3D008C5C000": "ALZPA", "010014E00DB56000": "AY6HA"
            ]

            if let tid = titleId, let gid = gameIdMap[tid] {
                for sub in ["coverHQ", "coverfullHQ", "coverM", "cover"] {
                    for reg in ["US", "EN", "JA", "CA"] {
                        if let u = URL(string: "https://art.gametdb.com/switch/\(sub)/\(reg)/\(gid).jpg") {
                            candidates.append(u)
                        }
                    }
                }
            }
            
            // 2. Raw GitHub repository images
            let slugs = self.generateSlugs(from: gameName)
            for slug in slugs {
                if let u = URL(string: "https://raw.githubusercontent.com/seedds/nxnative/main/docs/images/\(slug).jpg") {
                    candidates.append(u)
                }
            }

            // 3. Tinfoil CDN
            if let tid = titleId, tid.count >= 16 {
                if let u = URL(string: "https://tinfoil.media/ti/\(tid)/512/512/") {
                    candidates.append(u)
                }
            }

            for url in candidates {
                if let data = try? Data(contentsOf: url), let img = NSImage(data: data) {
                    if let tid = titleId {
                        let target = self.cacheDirectory.appendingPathComponent("\(tid).jpg")
                        try? data.write(to: target)
                    }
                    if let appURL = targetAppURL {
                        let resources = appURL.appendingPathComponent("Contents/Resources")
                        let coverFile = resources.appendingPathComponent("cover.jpg")
                        if self.fileManager.fileExists(atPath: resources.path) && !self.fileManager.fileExists(atPath: coverFile.path) {
                            try? data.write(to: coverFile)
                        }
                    }
                    DispatchQueue.main.async {
                        self.loadedCovers[searchKey] = img
                    }
                    return
                }
            }
        }
    }
}
