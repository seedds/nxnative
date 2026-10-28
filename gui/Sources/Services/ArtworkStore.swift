import Foundation
import AppKit
import SwiftUI

public final class ArtworkStore: ObservableObject {
    public static let shared = ArtworkStore()

    private let memoryCache = NSCache<NSString, NSImage>()
    private let fileManager = FileManager.default
    private let artworkDir: URL
    private let localRepoDir: URL
    private var inFlightDownloads = Set<String>()

    private var galleryMapByTitle: [String: String] = [:]
    private var galleryMapByTid: [String: String] = [:]
    private var titledbMap: [String: [String: String]] = [:]

    @Published public var refreshTrigger: Int = 0

    private init() {
        let appSupport = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        artworkDir = appSupport.appendingPathComponent("nxnative/artwork", isDirectory: true)
        localRepoDir = URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images")
        try? fileManager.createDirectory(at: artworkDir, withIntermediateDirectories: true)
        memoryCache.countLimit = 80

        loadLocalGalleryIndex()
        loadTitleDatabaseIndex()
    }

    public func loadSquareIcon(for game: PortedGame) -> NSImage {
        let tid = (game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name))?.uppercased()
        let cacheKey = "icon_\(tid ?? game.uniqueKey)" as NSString
        if let cached = memoryCache.object(forKey: cacheKey) {
            return cached
        }

        // 1. Direct app bundle resources check (macOS .app)
        if game.platform == .macApp {
            let res = game.url.appendingPathComponent("Contents/Resources")
            for name in ["icon.png", "AppIcon.icns", "cover.jpg"] {
                let target = res.appendingPathComponent(name)
                if fileManager.fileExists(atPath: target.path), let img = NSImage(contentsOf: target) {
                    memoryCache.setObject(img, forKey: cacheKey)
                    return img
                }
            }
        }

        // 2. Official 1024x1024 square icon from local cache / repository
        if let tid = tid {
            let diskIcon = artworkDir.appendingPathComponent("\(tid).icon.jpg")
            if fileManager.fileExists(atPath: diskIcon.path), let img = NSImage(contentsOf: diskIcon) {
                memoryCache.setObject(img, forKey: cacheKey)
                return img
            }
            let repoIcon = localRepoDir.appendingPathComponent("icons/\(tid).jpg")
            if fileManager.fileExists(atPath: repoIcon.path), let img = NSImage(contentsOf: repoIcon) {
                memoryCache.setObject(img, forKey: cacheKey)
                return img
            }
        }

        // 3. Fallback to GameCoverService (portrait box art)
        if let cover = GameCoverService.shared.coverImage(for: game) {
            memoryCache.setObject(cover, forKey: cacheKey)
            return cover
        }

        // 4. Default fallback system workspace icon
        let wsIcon = NSWorkspace.shared.icon(forFile: game.url.path)
        wsIcon.size = NSSize(width: 512, height: 512)
        memoryCache.setObject(wsIcon, forKey: cacheKey)
        return wsIcon
    }

    public func loadWidescreenHero(for game: PortedGame) -> NSImage? {
        let tid = (game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name))?.uppercased()
        let cacheKey = "hero_\(tid ?? game.name)" as NSString
        if let cached = memoryCache.object(forKey: cacheKey) {
            return cached
        }

        // 1. Local disk cache in artwork/ or heroes repository
        if let tid = tid {
            let diskPath = artworkDir.appendingPathComponent("\(tid).jpg")
            if fileManager.fileExists(atPath: diskPath.path), let img = NSImage(contentsOf: diskPath) {
                memoryCache.setObject(img, forKey: cacheKey)
                return img
            }
            let repoHero = localRepoDir.appendingPathComponent("heroes/\(tid).jpg")
            if fileManager.fileExists(atPath: repoHero.path), let img = NSImage(contentsOf: repoHero) {
                memoryCache.setObject(img, forKey: cacheKey)
                return img
            }
        }

        // 2. Local docs images repository via gallery map or fuzzy match
        if let repoImage = findLocalRepoHero(for: game.name, titleId: tid) {
            memoryCache.setObject(repoImage, forKey: cacheKey)
            return repoImage
        }

        // 3. Async background fetch from official Nintendo eShop database
        if let tid = tid {
            fetchWidescreenHeroAsync(titleId: tid)
        }

        // 4. Immediate fallback: return cover art so screen is never dark/blank
        if let cover = GameCoverService.shared.coverImage(for: game) {
            return cover
        }

        return nil
    }

    private func findLocalRepoHero(for gameName: String, titleId: String?) -> NSImage? {
        if let tid = titleId, let filename = galleryMapByTid[tid] {
            let u = localRepoDir.appendingPathComponent(filename)
            if fileManager.fileExists(atPath: u.path), let img = NSImage(contentsOf: u) {
                return img
            }
        }

        let normName = normalizeString(gameName)
        if let filename = galleryMapByTitle[normName] {
            let u = localRepoDir.appendingPathComponent(filename)
            if fileManager.fileExists(atPath: u.path), let img = NSImage(contentsOf: u) {
                return img
            }
        }

        for (name, filename) in galleryMapByTitle {
            if normName.contains(name) || name.contains(normName) {
                let u = localRepoDir.appendingPathComponent(filename)
                if fileManager.fileExists(atPath: u.path), let img = NSImage(contentsOf: u) {
                    return img
                }
            }
        }

        return nil
    }

    private func loadLocalGalleryIndex() {
        let tsvPath = localRepoDir.appendingPathComponent("gallery.tsv")
        guard let content = try? String(contentsOf: tsvPath, encoding: .utf8) else { return }
        let lines = content.components(separatedBy: .newlines)
        for line in lines {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
            let parts = trimmed.components(separatedBy: "\t")
            guard parts.count >= 2 else { continue }
            let filename = parts[0]
            let name = parts[1]
            let cleanName = normalizeString(name)
            galleryMapByTitle[cleanName] = filename
            if parts.count >= 4 && parts[3].hasPrefix("0100") {
                galleryMapByTid[parts[3].uppercased()] = filename
            }
        }
    }

    private func loadTitleDatabaseIndex() {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            let candidates = [
                self.localRepoDir.appendingPathComponent("switch_titles_artwork.json"),
                self.artworkDir.appendingPathComponent("switch_titles_artwork.json")
            ]
            for url in candidates {
                if self.fileManager.fileExists(atPath: url.path),
                   let data = try? Data(contentsOf: url),
                   let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: [String: Any]] {
                    var map: [String: [String: String]] = [:]
                    for (tid, dict) in json {
                        var entry: [String: String] = [:]
                        if let b = dict["banner"] as? String { entry["banner"] = b }
                        if let i = dict["icon"] as? String { entry["icon"] = i }
                        if let s = dict["screenshot"] as? String { entry["screenshot"] = s }
                        map[tid.uppercased()] = entry
                    }
                    DispatchQueue.main.async {
                        self.titledbMap = map
                        self.refreshTrigger += 1
                    }
                    break
                }
            }
        }
    }

    private func fetchWidescreenHeroAsync(titleId: String) {
        guard !inFlightDownloads.contains(titleId) else { return }
        inFlightDownloads.insert(titleId)

        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            defer {
                DispatchQueue.main.async { self.inFlightDownloads.remove(titleId) }
            }

            var bannerUrlString = self.titledbMap[titleId]?["banner"]
            if bannerUrlString == nil {
                bannerUrlString = self.titledbMap[titleId]?["screenshot"]
            }

            guard let urlStr = bannerUrlString, let url = URL(string: urlStr),
                  let data = try? Data(contentsOf: url),
                  let img = NSImage(data: data) else {
                return
            }

            let destination = self.artworkDir.appendingPathComponent("\(titleId).jpg")
            try? data.write(to: destination, options: .atomic)

            if let iconUrlStr = self.titledbMap[titleId]?["icon"],
               let iconUrl = URL(string: iconUrlStr),
               !self.fileManager.fileExists(atPath: self.artworkDir.appendingPathComponent("\(titleId).icon.jpg").path),
               let iconData = try? Data(contentsOf: iconUrl) {
                let iconDest = self.artworkDir.appendingPathComponent("\(titleId).icon.jpg")
                try? iconData.write(to: iconDest, options: .atomic)
            }

            DispatchQueue.main.async {
                self.memoryCache.setObject(img, forKey: "hero_\(titleId)" as NSString)
                self.refreshTrigger += 1
            }
        }
    }

    private func normalizeString(_ str: String) -> String {
        str.folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .joined()
    }
}
