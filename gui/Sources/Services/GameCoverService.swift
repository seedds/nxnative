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
        preloadLocalCovers()
    }

    private func preloadLocalCovers() {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            let coversDir = URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images/covers")
            guard let files = try? self.fileManager.contentsOfDirectory(at: coversDir, includingPropertiesForKeys: nil) else { return }
            
            var preloaded: [String: NSImage] = [:]
            for file in files where file.pathExtension.lowercased() == "jpg" || file.pathExtension.lowercased() == "png" {
                let tid = file.deletingPathExtension().lastPathComponent.uppercased()
                if let img = NSImage(contentsOf: file), img.size.height >= img.size.width * 1.15 {
                    preloaded[tid] = img
                }
            }
            DispatchQueue.main.async {
                self.loadedCovers.merge(preloaded) { current, _ in current }
            }
        }
    }

    public func customCoverURL(for game: PortedGame) -> URL {
        let titleId = game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name)
        let key = (titleId ?? game.uniqueKey).uppercased()
        return cacheDirectory.appendingPathComponent("\(key).custom.jpg")
    }

    public func hasCustomCover(for game: PortedGame) -> Bool {
        fileManager.fileExists(atPath: customCoverURL(for: game).path)
    }

    public func setCustomCover(imageURL: URL, for game: PortedGame) -> Bool {
        guard let image = NSImage(contentsOf: imageURL) else { return false }
        let titleId = game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name)
        let key = titleId ?? game.uniqueKey
        let dest = customCoverURL(for: game)

        try? fileManager.removeItem(at: dest)
        guard let tiff = image.tiffRepresentation,
              let rep = NSBitmapImageRep(data: tiff),
              let jpgData = rep.representation(using: .jpeg, properties: [:]) else { return false }
        
        do {
            try jpgData.write(to: dest)
            if game.url.pathExtension.lowercased() == "app" {
                let res = game.url.appendingPathComponent("Contents/Resources")
                let bundleCover = res.appendingPathComponent("cover.jpg")
                if fileManager.fileExists(atPath: res.path) {
                    try? jpgData.write(to: bundleCover)
                }
            }
            DispatchQueue.main.async {
                self.loadedCovers[key] = image
                if let tid = titleId { self.loadedCovers[tid.uppercased()] = image }
                self.objectWillChange.send()
            }
            return true
        } catch {
            return false
        }
    }

    public func resetCustomCover(for game: PortedGame) {
        let titleId = game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name)
        let key = titleId ?? game.uniqueKey
        let dest = customCoverURL(for: game)
        try? fileManager.removeItem(at: dest)

        if game.url.pathExtension.lowercased() == "app" {
            let bundleCover = game.url.appendingPathComponent("Contents/Resources/cover.jpg")
            try? fileManager.removeItem(at: bundleCover)
        }

        loadedCovers.removeValue(forKey: key)
        if let tid = titleId { loadedCovers.removeValue(forKey: tid.uppercased()) }
        _ = coverImage(for: game)
        DispatchQueue.main.async {
            self.objectWillChange.send()
        }
    }

    public func coverImage(for game: PortedGame) -> NSImage? {
        let titleId = game.titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: game.name)
        let key = titleId ?? game.uniqueKey
        
        if let memoryImage = loadedCovers[key] {
            return memoryImage
        }

        // 1. User custom cover
        let customFile = customCoverURL(for: game)
        if fileManager.fileExists(atPath: customFile.path), let img = NSImage(contentsOf: customFile) {
            loadedCovers[key] = img
            return img
        }

        // 2. Check game bundle's Contents/Resources for cover art (only if portrait)
        let bundleResources = game.url.appendingPathComponent("Contents/Resources")
        for filename in ["cover.jpg", "cover.png", "coverHQ.jpg", "boxart.jpg"] {
            let bundleCover = bundleResources.appendingPathComponent(filename)
            if fileManager.fileExists(atPath: bundleCover.path),
               let img = NSImage(contentsOf: bundleCover),
               img.size.height >= img.size.width * 1.05 {
                loadedCovers[key] = img
                return img
            }
        }

        // 3. Check disk cache in ~/Library/Application Support/nxnative/covers/<TitleID>.jpg
        if let tid = titleId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid.uppercased()).jpg")
            if fileManager.fileExists(atPath: cached.path),
               let img = NSImage(contentsOf: cached),
               img.size.height >= img.size.width * 1.05 {
                loadedCovers[key] = img
                injectCoverIfPossible(sourceImageURL: cached, targetAppURL: game.url)
                return img
            }
        }

        // 4. Check local docs/images/covers/ repository for matching portrait box art
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

        fetchOfficialCoverAsync(titleId: titleId?.uppercased(), gameName: game.name, targetAppURL: game.url)
        return NSWorkspace.shared.icon(forFile: game.url.path)
    }

    public func coverImage(titleId: String?, gameName: String) -> NSImage? {
        let resolvedId = titleId ?? CompatibilityService.shared.findTitleId(forGameTitle: gameName)
        let key = (resolvedId ?? gameName).uppercased()
        
        if let memoryImage = loadedCovers[key] {
            return memoryImage
        }
        
        if let tid = resolvedId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid.uppercased()).jpg")
            if fileManager.fileExists(atPath: cached.path),
               let img = NSImage(contentsOf: cached),
               img.size.height >= img.size.width * 1.05 {
                loadedCovers[key] = img
                return img
            }
        }
        
        if let localCover = findLocalRepoCover(forGameName: gameName, titleId: resolvedId) {
            if let img = NSImage(contentsOf: localCover), img.size.height >= img.size.width * 1.15 {
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
        let coversDir = URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images/covers")
        if let tid = titleId, !tid.isEmpty {
            for ext in ["jpg", "png", "jpeg"] {
                let byId = coversDir.appendingPathComponent("\(tid.uppercased()).\(ext)")
                if fileManager.fileExists(atPath: byId.path) { return byId }
            }
        }
        return nil
    }

    private func fetchOfficialCoverAsync(titleId: String?, gameName: String, targetAppURL: URL?) {
        let searchKey = (titleId ?? gameName).uppercased()
        guard !inFlightDownloads.contains(searchKey) else { return }
        inFlightDownloads.insert(searchKey)

        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            defer {
                DispatchQueue.main.async { self.inFlightDownloads.remove(searchKey) }
            }

            var candidates: [URL] = []
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
                "0100A3D008C5C000": "ALZXA", "01004AB00A260000": "AK63B",
                "01009D901BC56000": "BDLWA", "010020D01AD24000": "BCCHA"
            ]

            if let tid = titleId, let gid = gameIdMap[tid] {
                for sub in ["coverHQ", "coverfullHQ", "coverM"] {
                    for reg in ["US", "EN", "JA", "CA"] {
                        if let u = URL(string: "https://art.gametdb.com/switch/\(sub)/\(reg)/\(gid).jpg") {
                            candidates.append(u)
                        }
                    }
                }
            }

            for url in candidates {
                if let data = try? Data(contentsOf: url),
                   let img = NSImage(data: data),
                   img.size.height >= img.size.width * 1.1 {
                    if let tid = titleId {
                        let target = self.cacheDirectory.appendingPathComponent("\(tid).jpg")
                        try? data.write(to: target)
                    }
                    if let appURL = targetAppURL {
                        let res = appURL.appendingPathComponent("Contents/Resources")
                        let coverFile = res.appendingPathComponent("cover.jpg")
                        if self.fileManager.fileExists(atPath: res.path) && !self.fileManager.fileExists(atPath: coverFile.path) {
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
