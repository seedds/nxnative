import Foundation
import AppKit
import SwiftUI

public final class GameCoverService: ObservableObject {
    public static let shared = GameCoverService()

    private let memoryCache = NSCache<NSString, NSImage>()
    private let fileManager = FileManager.default
    private let cacheDirectory: URL
    private let coversRepoDirectory: URL
    private let lock = NSLock()

    private var inFlightDownloads = Set<String>()
    private var failedLookups = Set<String>()

    @Published public var refreshRevision: Int = 0

    /// Verified Title ID -> GameTDB 5-letter IDs (checked against official switchtdb.txt)
    public static var verifiedGameTDBIds: [String: [String]] {
        GameTDBVerifiedDatabase.ids
    }

    private init() {
        let appSup = fileManager.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/nxnative/covers")
        try? fileManager.createDirectory(at: appSup, withIntermediateDirectories: true)
        self.cacheDirectory = appSup

        let detected = SystemDetector.shared.detectNxnativeDirectory().path
        let repoCovers = URL(fileURLWithPath: detected).appendingPathComponent("docs/images/covers")
        if fileManager.fileExists(atPath: repoCovers.path) {
            self.coversRepoDirectory = repoCovers
        } else {
            self.coversRepoDirectory = URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images/covers")
        }

        // Expanded memory bounds for whole library/compatibility list: 512 images or 128 MB max
        memoryCache.countLimit = 512
        memoryCache.totalCostLimit = 128 * 1024 * 1024
    }

    public func purgeMemoryCaches() {
        memoryCache.removeAllObjects()
    }

    private func cacheImage(_ img: NSImage, forKeys keys: [String]) {
        let approxBytes = Int(img.size.width * img.size.height * 4)
        for key in keys {
            memoryCache.setObject(img, forKey: key.uppercased() as NSString, cost: max(approxBytes, 1024))
        }
    }

    private func getCachedImage(forKeys keys: [String]) -> NSImage? {
        for key in keys {
            if let cached = memoryCache.object(forKey: key.uppercased() as NSString) {
                return cached
            }
        }
        return nil
    }

    public func customCoverURL(for game: PortedGame) -> URL {
        let titleId = (game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name))?.uppercased()
        let key = (titleId ?? game.uniqueKey).uppercased()
        return cacheDirectory.appendingPathComponent("\(key).custom.jpg")
    }

    public func hasCustomCover(for game: PortedGame) -> Bool {
        fileManager.fileExists(atPath: customCoverURL(for: game).path)
    }

    public func setCustomCover(imageURL: URL, for game: PortedGame) -> Bool {
        guard let image = ImageDownsampler.downsample(url: imageURL, maxPixelSize: 320) ?? NSImage(contentsOf: imageURL) else { return false }
        let titleId = (game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name))?.uppercased()
        let key = (titleId ?? game.uniqueKey).uppercased()
        let dest = customCoverURL(for: game)

        guard let tiffData = image.tiffRepresentation,
              let bitmap = NSBitmapImageRep(data: tiffData),
              let jpgData = bitmap.representation(using: .jpeg, properties: [.compressionFactor: 0.90]) else {
            return false
        }

        do {
            try jpgData.write(to: dest, options: .atomic)
            injectCoverIfPossible(sourceImageData: jpgData, targetAppURL: game.url)

            var keysToCache = [key]
            if let tid = titleId { keysToCache.append(tid) }
            cacheImage(image, forKeys: keysToCache)

            DispatchQueue.main.async {
                self.refreshRevision += 1
                self.objectWillChange.send()
                ArtworkStore.shared.notifyArtworkUpdated(for: key, titleId: titleId)
            }
            return true
        } catch {
            return false
        }
    }

    public func resetCustomCover(for game: PortedGame) {
        let titleId = (game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name))?.uppercased()
        let key = (titleId ?? game.uniqueKey).uppercased()
        let dest = customCoverURL(for: game)
        try? fileManager.removeItem(at: dest)

        if game.url.pathExtension.lowercased() == "app" {
            let bundleCover = game.url.appendingPathComponent("Contents/Resources/cover.jpg")
            try? fileManager.removeItem(at: bundleCover)
        }

        memoryCache.removeObject(forKey: key as NSString)
        if let tid = titleId {
            memoryCache.removeObject(forKey: tid as NSString)
        }

        _ = coverImage(for: game)
        DispatchQueue.main.async {
            self.refreshRevision += 1
            self.objectWillChange.send()
            ArtworkStore.shared.notifyArtworkUpdated(for: key, titleId: titleId)
        }
    }

    public func coverImage(for game: PortedGame) -> NSImage? {
        let titleId = (game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name))?.uppercased()
        let key = (titleId ?? game.uniqueKey).uppercased()
        var lookupKeys = [key]
        if let tid = titleId { lookupKeys.append(tid) }

        if let memoryImage = getCachedImage(forKeys: lookupKeys) {
            return memoryImage
        }

        // 1. User custom cover
        let customFile = customCoverURL(for: game)
        if fileManager.fileExists(atPath: customFile.path),
           let img = ImageDownsampler.downsample(url: customFile, maxPixelSize: 320) ?? NSImage(contentsOf: customFile) {
            cacheImage(img, forKeys: lookupKeys)
            return img
        }

        // 2. Check game bundle's Contents/Resources for cover art (only if portrait)
        let bundleResources = game.url.appendingPathComponent("Contents/Resources")
        for filename in ["cover.jpg", "cover.png", "coverHQ.jpg", "boxart.jpg"] {
            let bundleCover = bundleResources.appendingPathComponent(filename)
            if fileManager.fileExists(atPath: bundleCover.path),
               let img = ImageDownsampler.downsample(url: bundleCover, maxPixelSize: 320) ?? NSImage(contentsOf: bundleCover),
               img.size.height >= img.size.width * 1.05 {
                cacheImage(img, forKeys: lookupKeys)
                return img
            }
        }

        // 3. Check disk cache in ~/Library/Application Support/nxnative/covers/<TitleID>.jpg
        if let tid = titleId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid).jpg")
            if fileManager.fileExists(atPath: cached.path),
               let img = ImageDownsampler.downsample(url: cached, maxPixelSize: 320) ?? NSImage(contentsOf: cached),
               img.size.height >= img.size.width * 1.05 {
                cacheImage(img, forKeys: lookupKeys)
                injectCoverIfPossible(sourceImageURL: cached, targetAppURL: game.url)
                return img
            }
        }

        // 4. Check local docs/images/covers/ repository for matching portrait box art
        if let localCover = findLocalRepoCover(forGameName: game.name, titleId: titleId) {
            if let img = ImageDownsampler.downsample(url: localCover, maxPixelSize: 320) ?? NSImage(contentsOf: localCover) {
                cacheImage(img, forKeys: lookupKeys)
                if let tid = titleId, !tid.isEmpty {
                    let cacheTarget = cacheDirectory.appendingPathComponent("\(tid).jpg")
                    if !fileManager.fileExists(atPath: cacheTarget.path) {
                        try? fileManager.copyItem(at: localCover, to: cacheTarget)
                    }
                }
                injectCoverIfPossible(sourceImageURL: localCover, targetAppURL: game.url)
                return img
            }
        }

        // 5. Asynchronously fetch from online databases (GameTDB, Libretro, Nintendo CDN)
        fetchOfficialCoverAsync(titleId: titleId, gameName: game.name, targetAppURL: game.url, gameKey: key)
        return nil
    }

    public func ensureCover(for game: PortedGame) {
        _ = coverImage(for: game)
    }

    public func coverImage(titleId: String?, gameName: String) -> NSImage? {
        let resolvedId = (titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: gameName))?.uppercased()
        let key = (resolvedId ?? gameName).uppercased()
        var lookupKeys = [key]
        if let tid = resolvedId { lookupKeys.append(tid) }

        if let memoryImage = getCachedImage(forKeys: lookupKeys) {
            return memoryImage
        }

        if let tid = resolvedId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid).jpg")
            if fileManager.fileExists(atPath: cached.path),
               let img = ImageDownsampler.downsample(url: cached, maxPixelSize: 320) ?? NSImage(contentsOf: cached),
               img.size.height >= img.size.width * 1.05 {
                cacheImage(img, forKeys: lookupKeys)
                return img
            }
        }

        if let localCover = findLocalRepoCover(forGameName: gameName, titleId: resolvedId) {
            if let img = ImageDownsampler.downsample(url: localCover, maxPixelSize: 320) ?? NSImage(contentsOf: localCover),
               img.size.height >= img.size.width * 1.05 {
                cacheImage(img, forKeys: lookupKeys)
                return img
            }
        }

        fetchOfficialCoverAsync(titleId: resolvedId, gameName: gameName, targetAppURL: nil, gameKey: key)
        return nil
    }

    public func injectCoverIntoBundle(at appURL: URL, gameName: String, titleId: String?) {
        let resolvedId = (titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: gameName))?.uppercased()
        if let localCover = findLocalRepoCover(forGameName: gameName, titleId: resolvedId) {
            injectCoverIfPossible(sourceImageURL: localCover, targetAppURL: appURL)
        } else if let tid = resolvedId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid).jpg")
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

    private func injectCoverIfPossible(sourceImageData: Data, targetAppURL: URL) {
        guard targetAppURL.pathExtension.lowercased() == "app" else { return }
        let resources = targetAppURL.appendingPathComponent("Contents/Resources")
        let targetCover = resources.appendingPathComponent("cover.jpg")

        if !fileManager.fileExists(atPath: targetCover.path) && fileManager.fileExists(atPath: resources.path) {
            try? sourceImageData.write(to: targetCover)
        }
    }

    private func findLocalRepoCover(forGameName name: String, titleId: String?) -> URL? {
        if let tid = titleId, !tid.isEmpty {
            for ext in ["jpg", "png", "jpeg"] {
                let byId = coversRepoDirectory.appendingPathComponent("\(tid.uppercased()).\(ext)")
                if fileManager.fileExists(atPath: byId.path),
                   let img = ImageDownsampler.downsample(url: byId, maxPixelSize: 320) ?? NSImage(contentsOf: byId),
                   img.size.height >= img.size.width * 1.05 {
                    return byId
                }
            }
        }

        let imagesDir = coversRepoDirectory.deletingLastPathComponent()
        let slug = slugify(name)
        if !slug.isEmpty {
            for ext in ["jpg", "png", "jpeg"] {
                let bySlug = imagesDir.appendingPathComponent("\(slug).\(ext)")
                if fileManager.fileExists(atPath: bySlug.path),
                   let img = ImageDownsampler.downsample(url: bySlug, maxPixelSize: 320) ?? NSImage(contentsOf: bySlug),
                   img.size.height >= img.size.width * 1.05 {
                    return bySlug
                }
            }
        }

        return nil
    }

    private func slugify(_ str: String) -> String {
        let folded = str.folding(options: [.diacriticInsensitive, .caseInsensitive], locale: .current)
        let allowed = CharacterSet.alphanumerics
        let clean = folded.unicodeScalars.map { allowed.contains($0) ? Character($0) : "-" }
        return String(clean)
            .components(separatedBy: "-")
            .filter { !$0.isEmpty }
            .joined(separator: "-")
            .lowercased()
    }

    private lazy var urlSession: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 3.0
        config.timeoutIntervalForResource = 6.0
        config.httpMaximumConnectionsPerHost = 4
        return URLSession(configuration: config)
    }()

    private var updateTimer: Timer? = nil
    private func scheduleBatchUpdateNotification() {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.updateTimer?.invalidate()
            self.updateTimer = Timer.scheduledTimer(withTimeInterval: 0.15, repeats: false) { [weak self] _ in
                self?.refreshRevision += 1
                self?.objectWillChange.send()
            }
        }
    }

    private func fetchOfficialCoverAsync(titleId: String?, gameName: String, targetAppURL: URL?, gameKey: String? = nil) {
        let tidUpper = titleId?.uppercased()
        let searchKey = (gameKey ?? tidUpper ?? gameName).uppercased()

        lock.lock()
        if inFlightDownloads.contains(searchKey) || failedLookups.contains(searchKey) {
            lock.unlock()
            return
        }
        inFlightDownloads.insert(searchKey)
        lock.unlock()

        var candidates: [URL] = []

        // 1. High-Probability GameTDB Box Arts (Priority: US, EN, JA)
        if let tid = tidUpper {
            let gids = Self.verifiedGameTDBIds[tid] ?? []
            for gid in gids {
                for sub in ["coverHQ", "cover"] {
                    for reg in ["US", "EN", "JA"] {
                        if let u = URL(string: "https://art.gametdb.com/switch/\(sub)/\(reg)/\(gid).jpg") {
                            candidates.append(u)
                        }
                    }
                }
            }
        }

        // 2. High-Probability Libretro Named_Boxarts
        let cleanTitle = gameName
            .replacingOccurrences(of: ":", with: " -")
            .replacingOccurrences(of: "\u{2122}", with: "")
            .replacingOccurrences(of: "\u{00AE}", with: "")
        for suffix in ["", " (World)", " (USA)"] {
            if let encoded = "\(cleanTitle)\(suffix).png".addingPercentEncoding(withAllowedCharacters: .urlPathAllowed),
               let libretroURL = URL(string: "https://thumbnails.libretro.com/Nintendo%20-%20Switch/Named_Boxarts/\(encoded)") {
                candidates.append(libretroURL)
            }
        }

        // 3. Nintendo Official eShop Artwork
        if let tid = tidUpper, let artwork = ArtworkStore.shared.artworkUrls(for: tid) {
            if let iconUrl = artwork.icon {
                candidates.append(iconUrl)
            }
            if let bannerUrl = artwork.banner {
                candidates.append(bannerUrl)
            }
        }

        guard !candidates.isEmpty else {
            lock.lock()
            inFlightDownloads.remove(searchKey)
            lock.unlock()
            return
        }

        tryFetchCandidate(candidates: candidates, index: 0) { [weak self] data, img in
            guard let self = self else { return }
            defer {
                self.lock.lock()
                self.inFlightDownloads.remove(searchKey)
                self.lock.unlock()
            }

            guard let data = data, let img = img else {
                self.lock.lock()
                self.failedLookups.insert(searchKey)
                self.lock.unlock()
                return
            }

            if let tid = tidUpper {
                let target = self.cacheDirectory.appendingPathComponent("\(tid).jpg")
                try? data.write(to: target, options: .atomic)
            }
            if let appURL = targetAppURL {
                self.injectCoverIfPossible(sourceImageData: data, targetAppURL: appURL)
            }

            var keysToCache = [searchKey]
            if let tid = tidUpper { keysToCache.append(tid) }
            self.cacheImage(img, forKeys: keysToCache)

            DispatchQueue.main.async {
                self.scheduleBatchUpdateNotification()
                ArtworkStore.shared.notifyArtworkUpdated(for: searchKey, titleId: tidUpper)
            }
        }
    }

    private func tryFetchCandidate(
        candidates: [URL],
        index: Int,
        completion: @escaping (Data?, NSImage?) -> Void
    ) {
        guard index < candidates.count else {
            completion(nil, nil)
            return
        }
        let url = candidates[index]
        let task = urlSession.dataTask(with: url) { [weak self] data, response, _ in
            guard let self = self else { return }
            if let http = response as? HTTPURLResponse, http.statusCode == 200,
               let d = data, !d.isEmpty,
               let img = ImageDownsampler.downsample(data: d, maxPixelSize: 320) ?? NSImage(data: d),
               img.isValid, img.size.height >= img.size.width * 1.05 {
                completion(d, img)
            } else {
                self.tryFetchCandidate(candidates: candidates, index: index + 1, completion: completion)
            }
        }
        task.resume()
    }
}
