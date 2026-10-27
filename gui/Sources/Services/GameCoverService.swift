import Foundation
import AppKit
import SwiftUI

public final class GameCoverService: ObservableObject {
    public static let shared = GameCoverService()

    @Published public var loadedCovers: [String: NSImage] = [:]
    private let fileManager = FileManager.default
    private let cacheDirectory: URL
    private var inFlightDownloads = Set<String>()

    /// Verified Title ID -> GameTDB 5-letter IDs (checked against official switchtdb.txt)
    public static let verifiedGameTDBIds: [String: [String]] = [
        "0100000000010000": ["AAACA", "AAACB"], // Super Mario Odyssey
        "0100000011D90000": ["AX7QA"],          // Pokémon Brilliant Diamond
        "010005501E68C000": ["BG9VA"],          // Ace Attorney Investigations Collection
        "010013C00E930000": ["AUSUA"],          // Hollow Knight: Silksong
        "010014E00DB56000": ["ATPMB", "ATPMA"], // Digimon Story Cyber Sleuth: Complete Edition
        "010015100B514000": ["AQMXA"],          // Super Mario Bros. Wonder
        "0100152000022000": ["AABPA", "AABPB"], // Mario Kart 8 Deluxe
        "010020400E1C2000": ["AEK3B", "AEK3A"], // Raging Loop
        "010020D01AD24000": ["BCCHA"],          // ANIMAL WELL
        "010028600EBDA000": ["AUZPA"],          // Super Mario 3D World + Bowser's Fury
        "010029B018432000": ["A8VEA"],          // Ghost Trick: Phantom Detective
        "01002B00111A2000": ["AXEAA", "AXEAB"], // Hyrule Warriors: Age of Calamity
        "01002B30028F6000": ["ACF3A"],          // Celeste
        "0100300012F2A000": ["AZRMA"],          // Advance Wars 1+2: Re-Boot Camp
        "0100317013770000": ["A2G7A", "A2G7B"], // Mario + Rabbids Sparks of Hope
        "010032E00E6E2000": ["AULSA"],          // Return of the Obra Dinn
        "0100346017304000": ["A7G4A"],          // Taiko Risshiden V DX
        "010036E00FB20000": ["AV3JA", "AV3JB"], // The Great Ace Attorney Chronicles
        "010038B015560000": ["A43UA"],          // FINAL FANTASY TACTICS - The Ivalice Chronicles
        "01003FC01670C000": ["A3RVA", "A3RVB", "A3RVC"], // 13 Sentinels: Aegis Rim
        "010040801A4C8000": ["BBLYA"],          // Blasphemous 2
        "010045C014650000": ["A3RVB", "A3RVA", "A3RVC"], // 13 Sentinels: Aegis Rim, Japanese release
        "01004A4010FEA000": ["AXB7A"],          // Bayonetta 3
        "01004AB00A260000": ["AK63B", "AK63A"], // DARK SOULS: REMASTERED
        "01004D300C5AE000": ["ARZGA"],          // Kirby and the Forgotten Land
        "010051701FB46000": ["BKCRA"],          // Monster Train 2
        "01005270232F2000": ["BPRWA"],          // Octopath Traveler 0
        "0100535012974000": ["AY9RA", "AY9R1"], // Hades
        "010054B01AD92000": ["BACCC", "BACCA", "BACCB"], // Unicorn Overlord, Asian release
        "010055D009F78000": ["ANVYA"],          // Fire Emblem: Three Houses
        "010056B015FE8000": ["A532A", "A532B"], // NieR:Automata The End of YoRHa Edition
        "010057D006492000": ["AGY7A", "AGY7B", "AGY7E"], // OCTOPATH TRAVELER
        "01005CA01580E000": ["A42UC", "A42UA"], // Persona 5 Royal
        "01006000040C2000": ["AEA2A", "AEA2B"], // Yoshi's Crafted World
        "0100606026F12000": ["BUCVA"],          // FINAL FANTASY RESONANCE
        "0100620012D6E000": ["AZLSA"],          // Big Brain Academy: Brain vs. Brain
        "010069401ADB8000": ["BACCA", "BACCB", "BACCC"], // Unicorn Overlord
        "010069C01AB82000": ["BBXAA", "BBXAB", "BBXAC"], // SHIN MEGAMI TENSEI V: Vengeance
        "01006A800016E000": ["AAABA"],          // Super Smash Bros. Ultimate
        "01006BB00C6F0000": ["AR3NA"],          // The Legend of Zelda: Link's Awakening
        "01006C300E9F0000": ["ALC7C", "ALC7E", "ALC7A"], // DRAGON QUEST XI S
        "01006F8002326000": ["ACBAA"],          // Animal Crossing: New Horizons
        "0100713016CCC000": ["A4EFA", "A4EFB", "A4EFC"], // AI: THE SOMNIUM FILES - nirvanA Initiative
        "01007300020FA000": ["AB48A"],          // ASTRAL CHAIN
        "010074F013262000": ["AZ3HA"],          // Xenoblade Chronicles 3
        "010076F0049A2000": ["AE99A"],          // Bayonetta
        "01007960049A0000": ["AE98A"],          // Bayonetta 2
        "01007BB017812000": ["A7Z4A"],          // Portal Companion Collection
        "01007EF00011E000": ["AAAAA"],          // The Legend of Zelda: Breath of the Wild
        "010086A022444000": ["BNQEA"],          // BALL x PIT
        "010089B00D09C000": ["ASRPB", "ASRPA", "ASRPD"], // AI: THE SOMNIUM FILES
        "01008BC024FDE000": ["AMNPA", "AMNPB", "ASB4A"], // STEINS;GATE RE:BOOT
        "01008C0016544000": ["A6G7A"],          // Sea of Stars
        "01008CF01BAAC000": ["BDGEA"],          // The Legend of Zelda: Echoes of Wisdom
        "01008D7016438000": ["A3RVC", "A3RVA", "A3RVB"], // 13 Sentinels: Aegis Rim (Asian release)
        "01008DB008C2C000": ["ALZBA"],          // Pokémon Shield
        "010093801237C000": ["AYL8A"],          // Metroid Dread
        "010099C022B96000": ["BN84A"],          // Super Mario Galaxy
        "01009BF0072D4000": ["AJH9A"],          // Captain Toad: Treasure Tracker
        "01009D901BC56000": ["BDLWA"],          // Donkey Kong Country Returns HD
        "0100A00019DE0000": ["BA3FA"],          // Hades II
        "0100A3501946E000": ["A98RA"],          // OCTOPATH TRAVELER II
        "0100A3D008C5C000": ["ALZXA"],          // Pokémon Scarlet
        "0100A5C00D162000": ["ASVUA", "ASVUG"], // Cuphead
        "0100A6301214E000": ["AYFNA"],          // Fire Emblem Engage
        "0100A83022A22000": ["BN47A"],          // Absolum
        "0100A9D01C446000": ["BD9VA", "BD9VB"], // DRAGON QUEST VII Reimagined
        "0100ABF008968000": ["ALZAA"],          // Pokémon Sword
        "0100B04011742000": ["AXSEA", "AXSEB"], // Monster Hunter Rise
        "0100B7C00933A000": ["AMPYA"],          // Pikmin 4
        "0100B99019412000": ["A97PA"],          // Mario vs. Donkey Kong
        "0100BF00112C0000": ["AJ2AC"],          // Catherine: Full Body
        "0100C1F0051B6000": ["AFWTA"],          // Donkey Kong Country: Tropical Freeze
        "0100C2A01A730000": ["BBTCA", "BBTCC", "BBTCB"], // ROMANCE OF THE THREE KINGDOMS 8 REMAKE
        "0100CD801CE5E000": ["BEYKA"],          // Balatro
        "0100CE4010AAC000": ["AUYRA", "AUYRB", "AUYRC"], // FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition
        "0100D9F01D474000": ["BFLTA"],          // Rhythm Heaven Groove
        "0100DCA0064A6000": ["AG3JA"],          // Luigi's Mansion 3
        "0100E44016C22000": ["A6YSA"],          // Patrick's Parabox
        "0100E65002BB8000": ["ACPGA", "ACPGB"], // Stardew Valley
        "0100E95004038000": ["ADENB", "ADENA"], // Xenoblade Chronicles 2
        "0100ECD018EBE000": ["A9QDA"],          // Paper Mario: The Thousand-Year Door
        "0100F2C0115B6000": ["AXN7A"],          // The Legend of Zelda: Tears of the Kingdom
        "0100F43008C44000": ["ALZLA"],          // Pokémon Legends: Z-A
        "0100F5D01DA3A000": ["BF7QA"],          // Brain Memory Training Academy
        "0100FD8022DAA000": ["BPDLA"],          // Super Mario Galaxy 2
        "0100FF500E34A000": ["AUBQA"]           // Xenoblade Chronicles: Definitive Edition
    ]

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
                if fileManager.fileExists(atPath: byId.path),
                   let img = NSImage(contentsOf: byId),
                   img.size.height >= img.size.width * 1.05 {
                    return byId
                }
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

            if let tid = titleId, let gids = Self.verifiedGameTDBIds[tid] {
                for gid in gids {
                    for sub in ["coverHQ", "coverfullHQ", "coverM", "cover"] {
                        for reg in ["US", "EN", "JA", "AU", "FR", "DE", "ES", "KO", "ZH"] {
                            if let u = URL(string: "https://art.gametdb.com/switch/\(sub)/\(reg)/\(gid).jpg") {
                                candidates.append(u)
                            }
                        }
                    }
                }
            }

            // Secondary fallback: Libretro Nintendo Switch Named_Boxarts
            let cleanTitle = gameName.replacingOccurrences(of: ":", with: " -")
            for suffix in ["", " (World)", " (USA)", " (USA, Europe)", " (Japan)"] {
                if let encoded = "\(cleanTitle)\(suffix).png".addingPercentEncoding(withAllowedCharacters: .urlPathAllowed),
                   let libretroURL = URL(string: "https://thumbnails.libretro.com/Nintendo%20-%20Switch/Named_Boxarts/\(encoded)") {
                    candidates.append(libretroURL)
                }
            }

            for url in candidates {
                if let data = try? Data(contentsOf: url),
                   let img = NSImage(data: data),
                   img.size.height >= img.size.width * 1.05 {
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
                        if let tid = titleId { self.loadedCovers[tid] = img }
                    }
                    return
                }
            }
        }
    }
}
