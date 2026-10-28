import Foundation
import AppKit

public struct GameModItem: Identifiable, Equatable {
    public let id: String
    public let name: String
    public let folderURL: URL
    public var isEnabled: Bool
    public let hasCodePatches: Bool
    public let hasAssetPatches: Bool
}

public final class ModManagerService: ObservableObject {
    public static let shared = ModManagerService()
    private let fileManager = FileManager.default

    @Published public var refreshTrigger: Int = 0

    private init() {}

    public func modsDirectory(for titleId: String) -> URL {
        let appSupport = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = appSupport.appendingPathComponent("nxnative/\(titleId.uppercased())/mods", isDirectory: true)
        try? fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    public func discoverMods(for titleId: String) -> [GameModItem] {
        let dir = modsDirectory(for: titleId)
        guard let entries = try? fileManager.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.isDirectoryKey]) else {
            return []
        }

        var results: [GameModItem] = []
        for url in entries {
            var isDir: ObjCBool = false
            guard fileManager.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue else { continue }

            let rawName = url.lastPathComponent
            let isDisabled = rawName.hasSuffix(".disabled")
            let cleanName = isDisabled ? String(rawName.dropLast(9)) : rawName

            let romfs = url.appendingPathComponent("romfs")
            let exefs = url.appendingPathComponent("exefs")
            let hasAssets = fileManager.fileExists(atPath: romfs.path)
            let hasCode = fileManager.fileExists(atPath: exefs.path)

            results.append(
                GameModItem(
                    id: cleanName,
                    name: cleanName.replacingOccurrences(of: "_", with: " "),
                    folderURL: url,
                    isEnabled: !isDisabled,
                    hasCodePatches: hasCode,
                    hasAssetPatches: hasAssets
                )
            )
        }
        return results.sorted { $0.name < $1.name }
    }

    public func toggleMod(_ mod: GameModItem, in titleId: String) -> Bool {
        let parent = modsDirectory(for: titleId)
        let newName = mod.isEnabled ? "\(mod.id).disabled" : mod.id
        let targetURL = parent.appendingPathComponent(newName)

        do {
            try fileManager.moveItem(at: mod.folderURL, to: targetURL)
            DispatchQueue.main.async {
                self.refreshTrigger += 1
            }
            return true
        } catch {
            return false
        }
    }

    public func openModsFolder(for titleId: String) {
        let dir = modsDirectory(for: titleId)
        NSWorkspace.shared.open(dir)
    }
}
