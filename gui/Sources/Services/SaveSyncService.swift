import Foundation
import AppKit

public struct SaveComparisonInfo: Equatable {
    public let macSaveURL: URL
    public let macModificationDate: Date?
    public let macFileCount: Int
    public let hasDeviceSave: Bool
}

public final class SaveSyncService: ObservableObject {
    public static let shared = SaveSyncService()
    internal let fileManager = FileManager.default

    @Published public private(set) var isICloudDriveAvailable: Bool = false

    private init() {
        checkICloudAvailability()
    }

    public func checkICloudAvailability() {
        let docs = fileManager.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Mobile Documents/com~apple~CloudDocs", isDirectory: true)
        self.isICloudDriveAvailable = fileManager.fileExists(atPath: docs.path)
    }

    // MARK: - iCloud Root Directory
    public var iCloudRootURL: URL {
        fileManager.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Mobile Documents/com~apple~CloudDocs/nxnative_launcher", isDirectory: true)
    }

    public func ensureRootFolderConfigured() {
        guard isICloudDriveAvailable else { return }
        let root = iCloudRootURL
        if !fileManager.fileExists(atPath: root.path) {
            try? fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        }
        // Set official NxNative Launcher application icon on the root folder
        if let appIcon = NSApp.applicationIconImage {
            applyFolderIcon(image: appIcon, folderURL: root)
        }
    }

    // MARK: - Game iCloud Directories
    public func sanitizedGameName(_ game: PortedGame) -> String {
        let safe = game.name.replacingOccurrences(of: "[:/\\\\?*\"<>|]", with: " ", options: .regularExpression)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return safe.isEmpty ? (game.titleId ?? "UnknownGame") : safe
    }

    public func iCloudFolder(for game: PortedGame) -> URL {
        iCloudRootURL.appendingPathComponent(sanitizedGameName(game), isDirectory: true)
    }

    public func iCloudSaveDirectory(for game: PortedGame) -> URL {
        iCloudFolder(for: game).appendingPathComponent("save", isDirectory: true)
    }

    public func isCloudSyncActive(for game: PortedGame) -> Bool {
        guard let titleId = game.titleId, !titleId.isEmpty else { return false }
        let localDir = macSaveDirectory(for: titleId)
        
        var isDir: ObjCBool = false
        if fileManager.fileExists(atPath: localDir.path, isDirectory: &isDir) {
            if let dest = try? fileManager.destinationOfSymbolicLink(atPath: localDir.path) {
                return dest.contains("nxnative_launcher") || dest.contains("CloudDocs")
            }
        }
        
        let cloudDir = iCloudSaveDirectory(for: game)
        return fileManager.fileExists(atPath: cloudDir.path)
    }

    // MARK: - Safe Setup & Migration
    @discardableResult
    public func ensureCloudSaveConfigured(for game: PortedGame, cover: NSImage?) -> Result<URL, Error> {
        ensureRootFolderConfigured()
        let gameFolder = iCloudFolder(for: game)
        let cloudSaveDir = iCloudSaveDirectory(for: game)
        let cloudBackupsDir = gameFolder.appendingPathComponent("backups", isDirectory: true)

        do {
            try fileManager.createDirectory(at: cloudSaveDir, withIntermediateDirectories: true)
            try fileManager.createDirectory(at: cloudBackupsDir, withIntermediateDirectories: true)

            // Apply game's box art cover as custom folder icon in Finder and iCloud Drive
            if let coverImage = cover ?? GameCoverService.shared.coverImage(for: game) {
                applyFolderIcon(image: coverImage, folderURL: gameFolder)
            }

            guard let titleId = game.titleId, !titleId.isEmpty else {
                return .success(cloudSaveDir)
            }

            let localSave = macSaveDirectory(for: titleId)
            let isSymlink: Bool = (try? fileManager.destinationOfSymbolicLink(atPath: localSave.path)) != nil

            // If local directory exists and is NOT already a symlink, migrate existing saves
            if fileManager.fileExists(atPath: localSave.path) && !isSymlink {
                _ = backupMacSave(titleId: titleId)
                if let contents = try? fileManager.contentsOfDirectory(atPath: localSave.path) {
                    for item in contents {
                        let src = localSave.appendingPathComponent(item)
                        let dst = cloudSaveDir.appendingPathComponent(item)
                        if !fileManager.fileExists(atPath: dst.path) {
                            try? fileManager.copyItem(at: src, to: dst)
                        }
                    }
                }
                // Safely remove local folder to replace with symlink
                try? fileManager.removeItem(at: localSave)
            }

            // Create transparent POSIX symlink pointing local AppSupport to iCloud Drive
            if !fileManager.fileExists(atPath: localSave.path) {
                let localParent = localSave.deletingLastPathComponent()
                try? fileManager.createDirectory(at: localParent, withIntermediateDirectories: true)
                try? fileManager.createSymbolicLink(at: localSave, withDestinationURL: cloudSaveDir)
            }

            // Write DM_SAVE into env.txt
            updateEnvFileWithCloudSave(titleId: titleId, cloudSavePath: cloudSaveDir.path)

            DispatchQueue.main.async { self.objectWillChange.send() }
            return .success(cloudSaveDir)
        } catch {
            return .failure(error)
        }
    }

    private func updateEnvFileWithCloudSave(titleId: String, cloudSavePath: String) {
        let appSupport = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let envFile = appSupport.appendingPathComponent("nxnative/\(titleId.uppercased())/env.txt")
        var lines: [String] = []
        if let existing = try? String(contentsOf: envFile, encoding: .utf8) {
            lines = existing.components(separatedBy: .newlines).filter { !$0.hasPrefix("DM_SAVE=") }
        }
        lines.append("DM_SAVE=\(cloudSavePath)")
        try? lines.joined(separator: "\n").write(to: envFile, atomically: true, encoding: .utf8)
    }

    // MARK: - Folder Icon Customization
    public func createFolderIconWithCover(coverImage: NSImage) -> NSImage {
        let size = NSSize(width: 512, height: 512)
        let composite = NSImage(size: size)
        composite.lockFocus()

        // 1. Base macOS folder icon
        let folderIcon = NSWorkspace.shared.icon(for: .folder)
        folderIcon.size = size
        folderIcon.draw(in: NSRect(origin: .zero, size: size), from: .zero, operation: .sourceOver, fraction: 1.0)

        // 2. Front pocket badge positioning for standard macOS folder
        let badgeSize: CGFloat = 185
        let badgeX: CGFloat = (size.width - badgeSize) / 2
        let badgeY: CGFloat = 118
        let badgeRect = NSRect(x: badgeX, y: badgeY, width: badgeSize, height: badgeSize)
        let cornerRadius: CGFloat = 36
        let clipPath = NSBezierPath(roundedRect: badgeRect, xRadius: cornerRadius, yRadius: cornerRadius)

        // Drop shadow for the badge
        NSGraphicsContext.saveGraphicsState()
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.42)
        shadow.shadowOffset = NSSize(width: 0, height: -3)
        shadow.shadowBlurRadius = 7
        shadow.set()
        NSColor.black.withAlphaComponent(0.3).setFill()
        clipPath.fill()
        NSGraphicsContext.restoreGraphicsState()

        // Clip and draw cover image inside badge with aspect fill & centering (no squishing/stretching)
        NSGraphicsContext.saveGraphicsState()
        clipPath.addClip()
        let imgSize = coverImage.size
        if imgSize.width > 0 && imgSize.height > 0 {
            let scale = max(badgeRect.width / imgSize.width, badgeRect.height / imgSize.height)
            let scaledW = imgSize.width * scale
            let scaledH = imgSize.height * scale
            let drawX = badgeRect.origin.x + (badgeRect.width - scaledW) / 2
            let drawY = badgeRect.origin.y + (badgeRect.height - scaledH) / 2
            let drawRect = NSRect(x: drawX, y: drawY, width: scaledW, height: scaledH)
            coverImage.draw(in: drawRect, from: .zero, operation: .sourceOver, fraction: 1.0)
        } else {
            coverImage.draw(in: badgeRect, from: .zero, operation: .sourceOver, fraction: 1.0)
        }
        NSGraphicsContext.restoreGraphicsState()

        // Subtle specular border around badge
        NSGraphicsContext.saveGraphicsState()
        NSColor.white.withAlphaComponent(0.24).setStroke()
        clipPath.lineWidth = 1.2
        clipPath.stroke()
        NSGraphicsContext.restoreGraphicsState()

        composite.unlockFocus()
        return composite
    }

    @discardableResult
    public func applyFolderIcon(image: NSImage?, folderURL: URL) -> Bool {
        guard let img = image else { return false }
        let composite = createFolderIconWithCover(coverImage: img)
        return NSWorkspace.shared.setIcon(composite, forFile: folderURL.path, options: [])
    }

    public func revealCloudGameFolder(for game: PortedGame, cover: NSImage?) {
        _ = ensureCloudSaveConfigured(for: game, cover: cover)
        let folder = iCloudFolder(for: game)
        NSWorkspace.shared.activateFileViewerSelecting([folder])
    }

    // MARK: - Safety Snapshots
    @discardableResult
    public func createSafetySnapshot(for game: PortedGame, label: String = "auto") -> URL? {
        let cloudSave = iCloudSaveDirectory(for: game)
        guard fileManager.fileExists(atPath: cloudSave.path) else { return nil }

        let backupsDir = iCloudFolder(for: game).appendingPathComponent("backups", isDirectory: true)
        try? fileManager.createDirectory(at: backupsDir, withIntermediateDirectories: true)

        let timestamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let destination = backupsDir.appendingPathComponent("save_snapshot_\(timestamp)_\(label)")

        do {
            try fileManager.copyItem(at: cloudSave, to: destination)
            return destination
        } catch {
            return nil
        }
    }

    // MARK: - Local Save Compatibility
    public func macSaveDirectory(for titleId: String) -> URL {
        let appSupport = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let base = appSupport.appendingPathComponent("nxnative/\(titleId.uppercased())", isDirectory: true)
        let saveDir = base.appendingPathComponent("save", isDirectory: true)
        let workDir = base.appendingPathComponent("save.work", isDirectory: true)

        if fileManager.fileExists(atPath: saveDir.path) {
            return saveDir
        }
        if fileManager.fileExists(atPath: workDir.path) {
            return workDir
        }
        return saveDir
    }

    public func resolvedSaveDirectory(forTitleId titleId: String) -> URL {
        let localDir = macSaveDirectory(for: titleId)
        if let dest = try? fileManager.destinationOfSymbolicLink(atPath: localDir.path) {
            return URL(fileURLWithPath: dest)
        }
        return localDir
    }

    public func inspectMacSave(for titleId: String) -> SaveComparisonInfo {
        let dir = resolvedSaveDirectory(forTitleId: titleId)
        guard fileManager.fileExists(atPath: dir.path) else {
            return SaveComparisonInfo(macSaveURL: dir, macModificationDate: nil, macFileCount: 0, hasDeviceSave: false)
        }

        let contents = (try? fileManager.contentsOfDirectory(atPath: dir.path)) ?? []
        let newest = contents.compactMap { name -> Date? in
            let path = dir.appendingPathComponent(name).path
            return (try? fileManager.attributesOfItem(atPath: path)[.modificationDate]) as? Date
        }.max()

        return SaveComparisonInfo(
            macSaveURL: dir,
            macModificationDate: newest,
            macFileCount: contents.count,
            hasDeviceSave: false
        )
    }

    public func backupMacSave(titleId: String) -> URL? {
        let saveDir = resolvedSaveDirectory(forTitleId: titleId)
        guard fileManager.fileExists(atPath: saveDir.path) else { return nil }

        let backupRoot = saveDir.deletingLastPathComponent().appendingPathComponent("backups", isDirectory: true)
        try? fileManager.createDirectory(at: backupRoot, withIntermediateDirectories: true)

        let timestamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let destination = backupRoot.appendingPathComponent("save_backup_\(timestamp)")

        do {
            try fileManager.copyItem(at: saveDir, to: destination)
            return destination
        } catch {
            return nil
        }
    }
}
