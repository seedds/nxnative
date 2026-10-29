import Foundation
import AppKit
import SwiftUI

public final class ArtworkStore: ObservableObject {
    public static let shared = ArtworkStore()

    private let memoryCache = NSCache<NSString, NSImage>()
    private let fileManager = FileManager.default
    private let artworkDir: URL
    private let localRepoDir: URL
    private let lock = NSLock()

    private var inFlightDownloads = Set<String>()
    private var galleryMapByTitle: [String: String] = [:]
    private var galleryMapByTid: [String: String] = [:]
    private var resolvedTitleEntries: [String: [String: String]] = [:]

    private lazy var urlSession: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 4.0
        config.timeoutIntervalForResource = 8.0
        config.httpMaximumConnectionsPerHost = 4
        return URLSession(configuration: config)
    }()

    private var notifyDebounceTimer: Timer? = nil

    @Published public var refreshTrigger: Int = 0

    private init() {
        let appSupport = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        artworkDir = appSupport.appendingPathComponent("nxnative/artwork", isDirectory: true)
        
        let detected = SystemDetector.shared.detectNxnativeDirectory().path
        let repoBase = URL(fileURLWithPath: detected).appendingPathComponent("docs/images")
        if fileManager.fileExists(atPath: repoBase.path) {
            localRepoDir = repoBase
        } else {
            localRepoDir = URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images")
        }
        
        try? fileManager.createDirectory(at: artworkDir, withIntermediateDirectories: true)
        
        // Strict memory bounds: 36 images or 24 MB max
        memoryCache.countLimit = 36
        memoryCache.totalCostLimit = 24 * 1024 * 1024

        loadLocalGalleryIndex()
    }

    public func purgeMemoryCaches() {
        memoryCache.removeAllObjects()
    }

    private func cacheImage(_ img: NSImage, forKey key: NSString) {
        let approxBytes = Int(img.size.width * img.size.height * 4)
        memoryCache.setObject(img, forKey: key, cost: max(approxBytes, 1024))
    }

    public func loadSquareIcon(for game: PortedGame) -> NSImage {
        let tid = (game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name))?.uppercased()
        let cacheKey = "icon_\(tid ?? game.uniqueKey.uppercased())" as NSString
        if let cached = memoryCache.object(forKey: cacheKey) {
            return cached
        }

        // 1. User custom cover art has absolute top priority
        if GameCoverService.shared.hasCustomCover(for: game) {
            let customFile = GameCoverService.shared.customCoverURL(for: game)
            if let img = ImageDownsampler.downsample(url: customFile, maxPixelSize: 320) ?? NSImage(contentsOf: customFile) {
                cacheImage(img, forKey: cacheKey)
                return img
            }
        }

        // 2. Official 1024x1024 1:1 Nintendo Switch Square Icon (local cache or repository)
        if let tid = tid {
            let diskIcon = artworkDir.appendingPathComponent("\(tid).icon.jpg")
            if fileManager.fileExists(atPath: diskIcon.path),
               let img = ImageDownsampler.downsample(url: diskIcon, maxPixelSize: 320) ?? NSImage(contentsOf: diskIcon) {
                cacheImage(img, forKey: cacheKey)
                return img
            }
            let repoIcon = localRepoDir.appendingPathComponent("icons/\(tid).jpg")
            if fileManager.fileExists(atPath: repoIcon.path),
               let img = ImageDownsampler.downsample(url: repoIcon, maxPixelSize: 320) ?? NSImage(contentsOf: repoIcon) {
                cacheImage(img, forKey: cacheKey)
                return img
            }
            fetchSquareIconAsync(titleId: tid)
        }

        // 3. Direct app bundle resources check (macOS .app) for square icon or cover
        if game.platform == .macApp {
            let res = game.url.appendingPathComponent("Contents/Resources")
            for name in ["icon.png", "AppIcon.icns", "cover.jpg", "cover.png"] {
                let target = res.appendingPathComponent(name)
                if fileManager.fileExists(atPath: target.path),
                   let img = ImageDownsampler.downsample(url: target, maxPixelSize: 320) ?? NSImage(contentsOf: target) {
                    cacheImage(img, forKey: cacheKey)
                    return img
                }
            }
        }

        // 4. Authentic box art cover from GameCoverService (cropped 1:1 in UI until square icon is ready)
        if let cover = GameCoverService.shared.coverImage(for: game) {
            return cover
        }

        // 5. Trigger proactive background cover fetch so the card gets populated automatically
        GameCoverService.shared.ensureCover(for: game)

        // 6. Temporary fallback system workspace icon
        let wsIcon = NSWorkspace.shared.icon(forFile: game.url.path)
        wsIcon.size = NSSize(width: 256, height: 256)
        return wsIcon
    }

    public func loadSquareIcon(titleId: String?, gameName: String) -> NSImage? {
        let tid = (titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: gameName))?.uppercased()
        let cacheKey = "icon_\(tid ?? gameName.uppercased())" as NSString
        if let cached = memoryCache.object(forKey: cacheKey) {
            return cached
        }

        // 1. Official 1024x1024 1:1 Nintendo Switch Square Icon from local cache / repository
        if let tid = tid {
            let diskIcon = artworkDir.appendingPathComponent("\(tid).icon.jpg")
            if fileManager.fileExists(atPath: diskIcon.path),
               let img = ImageDownsampler.downsample(url: diskIcon, maxPixelSize: 320) ?? NSImage(contentsOf: diskIcon) {
                cacheImage(img, forKey: cacheKey)
                return img
            }
            let repoIcon = localRepoDir.appendingPathComponent("icons/\(tid).jpg")
            if fileManager.fileExists(atPath: repoIcon.path),
               let img = ImageDownsampler.downsample(url: repoIcon, maxPixelSize: 320) ?? NSImage(contentsOf: repoIcon) {
                cacheImage(img, forKey: cacheKey)
                return img
            }
            fetchSquareIconAsync(titleId: tid)
        }

        // 2. Authentic box art cover from GameCoverService
        if let cover = GameCoverService.shared.coverImage(titleId: tid, gameName: gameName) {
            return cover
        }

        return nil
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
            if fileManager.fileExists(atPath: diskPath.path),
               let img = ImageDownsampler.downsample(url: diskPath, maxPixelSize: 1280) ?? NSImage(contentsOf: diskPath) {
                cacheImage(img, forKey: cacheKey)
                return img
            }
            let repoHero = localRepoDir.appendingPathComponent("heroes/\(tid).jpg")
            if fileManager.fileExists(atPath: repoHero.path),
               let img = ImageDownsampler.downsample(url: repoHero, maxPixelSize: 1280) ?? NSImage(contentsOf: repoHero) {
                cacheImage(img, forKey: cacheKey)
                return img
            }
        }

        // 2. Local docs images repository via gallery map or fuzzy match
        if let repoImage = findLocalRepoHero(for: game.name, titleId: tid) {
            cacheImage(repoImage, forKey: cacheKey)
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
            if fileManager.fileExists(atPath: u.path),
               let img = ImageDownsampler.downsample(url: u, maxPixelSize: 1280) ?? NSImage(contentsOf: u) {
                return img
            }
        }

        let normName = normalizeString(gameName)
        if let filename = galleryMapByTitle[normName] {
            let u = localRepoDir.appendingPathComponent(filename)
            if fileManager.fileExists(atPath: u.path),
               let img = ImageDownsampler.downsample(url: u, maxPixelSize: 1280) ?? NSImage(contentsOf: u) {
                return img
            }
        }

        for (name, filename) in galleryMapByTitle {
            if normName.contains(name) || name.contains(normName) {
                let u = localRepoDir.appendingPathComponent(filename)
                if fileManager.fileExists(atPath: u.path),
                   let img = ImageDownsampler.downsample(url: u, maxPixelSize: 1280) ?? NSImage(contentsOf: u) {
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

    /// On-demand lookup of TitleID entry from switch_titles_artwork.json using memory mapping.
    /// Does not materialize the 29,000-entry JSON into heap.
    private func lookupTitleDbEntry(for titleId: String) -> [String: String]? {
        let tidUpper = titleId.uppercased()
        lock.lock()
        if let cached = resolvedTitleEntries[tidUpper] {
            lock.unlock()
            return cached
        }
        lock.unlock()

        let candidates = [
            self.localRepoDir.appendingPathComponent("switch_titles_artwork.json"),
            self.artworkDir.appendingPathComponent("switch_titles_artwork.json")
        ]
        guard let url = candidates.first(where: { fileManager.fileExists(atPath: $0.path) }),
              let fileData = try? Data(contentsOf: url, options: .mappedIfSafe) else {
            return nil
        }

        guard let needle = "\"\(tidUpper)\"".data(using: .utf8),
              let range = fileData.range(of: needle) else {
            return nil
        }

        let subdata = fileData[range.upperBound...]
        guard let openBraceIndex = subdata.firstIndex(of: UInt8(ascii: "{")) else {
            return nil
        }
        var depth = 0
        var closeBraceIndex: Data.Index? = nil
        for idx in openBraceIndex..<fileData.endIndex {
            let byte = fileData[idx]
            if byte == UInt8(ascii: "{") {
                depth += 1
            } else if byte == UInt8(ascii: "}") {
                depth -= 1
                if depth == 0 {
                    closeBraceIndex = idx
                    break
                }
            }
        }

        guard let endIdx = closeBraceIndex else { return nil }
        let objectSlice = fileData[openBraceIndex...endIdx]
        guard let obj = try? JSONSerialization.jsonObject(with: objectSlice) as? [String: Any] else {
            return nil
        }

        var result: [String: String] = [:]
        if let b = obj["banner"] as? String { result["banner"] = b }
        if let i = obj["icon"] as? String { result["icon"] = i }
        if let s = obj["screenshot"] as? String { result["screenshot"] = s }
        if let n = obj["name"] as? String { result["name"] = n }

        lock.lock()
        resolvedTitleEntries[tidUpper] = result
        lock.unlock()

        return result
    }

    private func fetchSquareIconAsync(titleId: String) {
        let iconFlightKey = "icon_\(titleId)"
        lock.lock()
        if inFlightDownloads.contains(iconFlightKey) {
            lock.unlock()
            return
        }
        inFlightDownloads.insert(iconFlightKey)
        lock.unlock()

        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            guard let entry = self.lookupTitleDbEntry(for: titleId),
                  let iconUrlStr = entry["icon"],
                  let url = URL(string: iconUrlStr) else {
                self.lock.lock()
                self.inFlightDownloads.remove(iconFlightKey)
                self.lock.unlock()
                return
            }

            let task = self.urlSession.dataTask(with: url) { [weak self] data, response, _ in
                guard let self = self else { return }
                defer {
                    self.lock.lock()
                    self.inFlightDownloads.remove(iconFlightKey)
                    self.lock.unlock()
                }
                guard let http = response as? HTTPURLResponse, http.statusCode == 200,
                      let data = data,
                      let img = ImageDownsampler.downsample(data: data, maxPixelSize: 320) ?? NSImage(data: data) else {
                    return
                }
                let iconDest = self.artworkDir.appendingPathComponent("\(titleId).icon.jpg")
                try? data.write(to: iconDest, options: .atomic)

                DispatchQueue.main.async {
                    self.cacheImage(img, forKey: "icon_\(titleId)" as NSString)
                    self.refreshTrigger += 1
                    self.objectWillChange.send()
                }
            }
            task.resume()
        }
    }

    private func fetchWidescreenHeroAsync(titleId: String) {
        let flightKey = "hero_\(titleId)"
        lock.lock()
        if inFlightDownloads.contains(flightKey) {
            lock.unlock()
            return
        }
        inFlightDownloads.insert(flightKey)
        lock.unlock()

        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            guard let entry = self.lookupTitleDbEntry(for: titleId) else {
                self.lock.lock()
                self.inFlightDownloads.remove(flightKey)
                self.lock.unlock()
                return
            }
            let bannerUrlString = entry["banner"] ?? entry["screenshot"]
            guard let urlStr = bannerUrlString, let url = URL(string: urlStr) else {
                self.lock.lock()
                self.inFlightDownloads.remove(flightKey)
                self.lock.unlock()
                return
            }

            let task = self.urlSession.dataTask(with: url) { [weak self] data, response, _ in
                guard let self = self else { return }
                defer {
                    self.lock.lock()
                    self.inFlightDownloads.remove(flightKey)
                    self.lock.unlock()
                }

                guard let http = response as? HTTPURLResponse, http.statusCode == 200,
                      let data = data,
                      let img = ImageDownsampler.downsample(data: data, maxPixelSize: 1280) ?? NSImage(data: data) else {
                    return
                }

                let destination = self.artworkDir.appendingPathComponent("\(titleId).jpg")
                try? data.write(to: destination, options: .atomic)

                if let iconUrlStr = entry["icon"],
                   let iconUrl = URL(string: iconUrlStr),
                   !self.fileManager.fileExists(atPath: self.artworkDir.appendingPathComponent("\(titleId).icon.jpg").path) {
                    let iconTask = self.urlSession.dataTask(with: iconUrl) { iconData, iconResp, _ in
                        if let iconHttp = iconResp as? HTTPURLResponse, iconHttp.statusCode == 200,
                           let iconD = iconData {
                            let iconDest = self.artworkDir.appendingPathComponent("\(titleId).icon.jpg")
                            try? iconD.write(to: iconDest, options: .atomic)
                        }
                    }
                    iconTask.resume()
                }

                DispatchQueue.main.async {
                    self.cacheImage(img, forKey: "hero_\(titleId)" as NSString)
                    self.refreshTrigger += 1
                }
            }
            task.resume()
        }
    }

    public func artworkUrls(for titleId: String) -> (icon: URL?, banner: URL?)? {
        let tid = titleId.uppercased()
        guard let entry = lookupTitleDbEntry(for: tid) else { return nil }
        let iconURL = entry["icon"].flatMap { URL(string: $0) }
        let bannerURL = (entry["banner"] ?? entry["screenshot"]).flatMap { URL(string: $0) }
        return (icon: iconURL, banner: bannerURL)
    }

    public func notifyArtworkUpdated(for key: String, titleId: String?) {
        let normalizedKey = key.uppercased()
        memoryCache.removeObject(forKey: "icon_\(normalizedKey)" as NSString)
        memoryCache.removeObject(forKey: "hero_\(normalizedKey)" as NSString)
        if let tid = titleId?.uppercased() {
            memoryCache.removeObject(forKey: "icon_\(tid)" as NSString)
            memoryCache.removeObject(forKey: "hero_\(tid)" as NSString)
        }
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.refreshTrigger += 1
            self.notifyDebounceTimer?.invalidate()
            self.notifyDebounceTimer = Timer.scheduledTimer(withTimeInterval: 0.15, repeats: false) { [weak self] _ in
                self?.objectWillChange.send()
            }
        }
    }

    public func clearCache(for game: PortedGame) {
        let tid = (game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name))?.uppercased()
        let iconKey = "icon_\(tid ?? game.uniqueKey.uppercased())" as NSString
        let heroKey = "hero_\(tid ?? game.uniqueKey.uppercased())" as NSString
        memoryCache.removeObject(forKey: iconKey)
        memoryCache.removeObject(forKey: heroKey)
        refreshTrigger += 1
        objectWillChange.send()
    }

    private func normalizeString(_ str: String) -> String {
        str.folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .joined()
    }
}
