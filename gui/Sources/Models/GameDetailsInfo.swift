import Foundation
import AppKit

public struct GameDLCItem: Identifiable, Hashable {
    public let id: String
    public let name: String
    public let index: Int
    public let sizeBytes: Int64
    public let folderPath: String
    
    public var formattedSize: String {
        ByteCountFormatter.string(fromByteCount: sizeBytes, countStyle: .file)
    }
}

public struct GameDetailsProfile: Equatable {
    public let gameId: UUID
    public let dlcItems: [GameDLCItem]
    public let saveDirectory: URL
    public let saveExists: Bool
    public let saveSizeBytes: Int64
    public let appBundleSizeBytes: Int64
    public let isExecutablePlayable: Bool
    
    public var formattedTotalSize: String {
        ByteCountFormatter.string(fromByteCount: appBundleSizeBytes, countStyle: .file)
    }
    
    public var formattedSaveSize: String {
        ByteCountFormatter.string(fromByteCount: saveSizeBytes, countStyle: .file)
    }
    
    public static func inspect(for game: PortedGame) -> GameDetailsProfile {
        let fm = FileManager.default
        let appURL = game.url
        
        // 1. Inspect DLCs in Contents/Resources/aoc
        var dlcs: [GameDLCItem] = []
        let aocDir = appURL.appendingPathComponent("Contents/Resources/aoc")
        if fm.fileExists(atPath: aocDir.path),
           let contents = try? fm.contentsOfDirectory(at: aocDir, includingPropertiesForKeys: [.fileSizeKey, .isDirectoryKey]) {
            for (idx, item) in contents.enumerated() {
                var isDir: ObjCBool = false
                if fm.fileExists(atPath: item.path, isDirectory: &isDir) {
                    let dlcSize = calculateSize(at: item)
                    let dlcName = item.lastPathComponent
                    dlcs.append(
                        GameDLCItem(
                            id: "\(game.uniqueKey)_dlc_\(idx)",
                            name: dlcName,
                            index: idx + 1,
                            sizeBytes: dlcSize,
                            folderPath: item.path
                        )
                    )
                }
            }
        }
        
        // 2. Resolve Save Directory
        let home = NSHomeDirectory()
        let saveDir: URL
        if let tid = game.titleId, !tid.isEmpty {
            saveDir = URL(fileURLWithPath: "\(home)/Library/Application Support/nxnative/\(tid)/save")
        } else if let bid = game.bundleId, !bid.isEmpty {
            saveDir = URL(fileURLWithPath: "\(home)/Library/Application Support/\(bid)/save")
        } else {
            let safe = game.name.replacingOccurrences(of: "[^a-zA-Z0-9._-]", with: "_", options: .regularExpression)
            saveDir = URL(fileURLWithPath: "\(home)/Library/Application Support/nxnative/\(safe)/save")
        }
        
        let saveExists = fm.fileExists(atPath: saveDir.path)
        let saveSize = saveExists ? calculateSize(at: saveDir) : 0
        
        return GameDetailsProfile(
            gameId: game.id,
            dlcItems: dlcs,
            saveDirectory: saveDir,
            saveExists: saveExists,
            saveSizeBytes: saveSize,
            appBundleSizeBytes: game.sizeBytes,
            isExecutablePlayable: game.isPlayableLocally
        )
    }
    
    private static func calculateSize(at url: URL) -> Int64 {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: url.path, isDirectory: &isDir) else { return 0 }
        
        if !isDir.boolValue {
            return (try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize.map { Int64($0) } ?? 0
        }
        
        guard let enumerator = fm.enumerator(at: url, includingPropertiesForKeys: [.fileSizeKey]) else { return 0 }
        var total: Int64 = 0
        for case let file as URL in enumerator {
            if let sz = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize {
                total += Int64(sz)
            }
        }
        return total
    }
}
