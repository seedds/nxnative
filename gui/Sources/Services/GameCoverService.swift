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
        let slugs = generateSlugs(from: name)
        let searchDirectories = [
            "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images",
            Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/images").path,
            Bundle.main.resourcePath ?? ""
        ]

        for dirPath in searchDirectories where !dirPath.isEmpty {
            let dirURL = URL(fileURLWithPath: dirPath)
            guard fileManager.fileExists(atPath: dirURL.path) else { continue }

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
        
        // Literal original slug without zelda transformation
        let slug2 = name.lowercased()
            .replacingOccurrences(of: "’", with: "")
            .replacingOccurrences(of: "'", with: "")
            .replacingOccurrences(of: ":", with: "")
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .joined(separator: "-")
        if slug2 != slug1 {
            results.append(slug2)
        }
        
        return results
    }

    private func fetchOfficialCoverAsync(titleId: String?, gameName: String, targetAppURL: URL?) {
        let searchKey = titleId ?? gameName
        guard !inFlightDownloads.contains(searchKey) else { return }
        inFlightDownloads.insert(searchKey)

        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            defer {
                DispatchQueue.main.async {
                    self.inFlightDownloads.remove(searchKey)
                }
            }

            var candidates: [URL] = []
            let slugs = self.generateSlugs(from: gameName)
            
            // 1. Raw GitHub repository images
            for slug in slugs {
                if let u = URL(string: "https://raw.githubusercontent.com/seedds/nxnative/main/docs/images/\(slug).jpg") {
                    candidates.append(u)
                }
                if let u = URL(string: "https://raw.githubusercontent.com/seedds/nxnative/main/docs/images/\(slug).png") {
                    candidates.append(u)
                }
            }

            // 2. Known GameTDB IDs
            let gameIdMap: [String: String] = [
                "0100F2C0115B6000": "AXN7A", // Zelda: Tears of the Kingdom
                "01007EF00011E000": "AAAAA", // Zelda: Breath of the Wild
                "0100DCA0064A6000": "ALWHA", // Super Mario 3D World + Bowser's Fury
                "0100000000010000": "AAACA", // Super Mario Odyssey
                "01002B00111A2000": "A3N9A", // Zelda: Link's Awakening
                "01006BB00C6F0000": "A3N9A", // Zelda: Link's Awakening (Revision)
                "01006F8002326000": "ANPHA", // Animal Crossing: New Horizons
                "0100152000022000": "AABPA"  // Mario Kart 8 Deluxe
            ]

            if let tid = titleId, let gid = gameIdMap[tid] {
                if let u1 = URL(string: "https://art.gametdb.com/switch/coverHQ/US/\(gid).jpg") { candidates.append(u1) }
                if let u2 = URL(string: "https://art.gametdb.com/switch/coverM/US/\(gid).jpg") { candidates.append(u2) }
            }

            // 3. Tinfoil CDN
            if let tid = titleId, tid.count >= 16 {
                if let u = URL(string: "https://tinfoil.media/ti/\(tid)/256/256/") {
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
