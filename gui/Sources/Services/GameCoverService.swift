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
        let key = game.titleId ?? game.uniqueKey
        if let memoryImage = loadedCovers[key] {
            return memoryImage
        }

        // 1. Check game bundle's Contents/Resources
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
        if let tid = game.titleId, !tid.isEmpty {
            let cached = cacheDirectory.appendingPathComponent("\(tid.uppercased()).jpg")
            if fileManager.fileExists(atPath: cached.path),
               let img = NSImage(contentsOf: cached) {
                loadedCovers[key] = img
                return img
            }

            // 3. Initiate async background download if not yet fetched
            fetchOfficialCoverAsync(titleId: tid.uppercased(), gameName: game.name)
        }

        // 4. Default to app icon
        return NSWorkspace.shared.icon(forFile: game.url.path)
    }

    private func fetchOfficialCoverAsync(titleId: String, gameName: String) {
        guard !inFlightDownloads.contains(titleId) else { return }
        inFlightDownloads.insert(titleId)

        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            defer {
                DispatchQueue.main.async {
                    self.inFlightDownloads.remove(titleId)
                }
            }

            // GameID mapping known for popular titles or derive from titledb
            let gameIdMap: [String: String] = [
                "0100F2C0115B6000": "AXN7A", // Zelda: Tears of the Kingdom
                "01007EF00011E000": "AAAAA", // Zelda: Breath of the Wild
                "0100DCA0064A6000": "ALWHA", // Super Mario 3D World + Bowser's Fury
                "0100000000010000": "AAACA", // Super Mario Odyssey
                "01002B00111A2000": "A3N9A", // Zelda: Link's Awakening
                "01006F8002326000": "ANPHA", // Animal Crossing: New Horizons
                "0100152000022000": "AABPA"  // Mario Kart 8 Deluxe
            ]

            var candidates: [URL] = []
            if let gid = gameIdMap[titleId] {
                if let u1 = URL(string: "https://art.gametdb.com/switch/coverHQ/US/\(gid).jpg") { candidates.append(u1) }
                if let u2 = URL(string: "https://art.gametdb.com/switch/coverM/US/\(gid).jpg") { candidates.append(u2) }
            }

            // Try candidate URLs
            for url in candidates {
                if let data = try? Data(contentsOf: url), let img = NSImage(data: data) {
                    let target = self.cacheDirectory.appendingPathComponent("\(titleId).jpg")
                    try? data.write(to: target)
                    DispatchQueue.main.async {
                        self.loadedCovers[titleId] = img
                    }
                    return
                }
            }
        }
    }
}
