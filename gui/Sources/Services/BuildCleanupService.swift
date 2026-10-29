import Foundation

public enum BuildFolderCategory: String, CaseIterable, Identifiable {
    case extracted = "Extracted Game Assets"
    case shaders = "Compiled Shader Caches & PSOs"
    case xcode = "Xcode Derived Data & Intermediate Bundles"
    case caches = "Temporary Extraction & Patch Caches"

    public var id: String { rawValue }

    public static func category(for entryName: String) -> BuildFolderCategory? {
        var base = entryName
        for suffix in [".key", ".partial"] where base.hasSuffix(suffix) {
            base = String(base.dropLast(suffix.count))
        }
        switch base {
        case "extract", "exefs": return .extracted
        case "shader", "pso": return .shaders
        case "xcode", "xcode-derived-data", "ipa": return .xcode
        case "tools", "tmp", "aoc", "font": return .caches
        default:
            if base.hasPrefix("patched_") || base.hasPrefix("nro_") {
                return .caches
            }
            return nil
        }
    }
}

public struct BuildStorageReport: Equatable {
    public let buildFolder: URL
    public var categorySizes: [BuildFolderCategory: Int64] = [:]
    public var preservedBytes: Int64 = 0

    public var totalReclaimableBytes: Int64 {
        categorySizes.values.reduce(0, +)
    }

    public var formattedReclaimable: String {
        ByteCountFormatter.string(fromByteCount: totalReclaimableBytes, countStyle: .file)
    }
}

public final class BuildCleanupService {
    public static let shared = BuildCleanupService()
    private let fileManager = FileManager.default

    private init() {}

    public func audit(buildFolder: URL) -> BuildStorageReport? {
        guard fileManager.fileExists(atPath: buildFolder.path) else { return nil }
        var report = BuildStorageReport(buildFolder: buildFolder)

        let contents = (try? fileManager.contentsOfDirectory(atPath: buildFolder.path)) ?? []
        for item in contents {
            let itemURL = buildFolder.appendingPathComponent(item)
            let size = folderSize(itemURL)

            if let cat = BuildFolderCategory.category(for: item) {
                report.categorySizes[cat, default: 0] += size
            } else {
                report.preservedBytes += size
            }
        }
        return report
    }

    public func clean(categories: Set<BuildFolderCategory>, in buildFolder: URL) -> Bool {
        guard fileManager.fileExists(atPath: buildFolder.path) else { return false }
        let contents = (try? fileManager.contentsOfDirectory(atPath: buildFolder.path)) ?? []

        for item in contents {
            if let cat = BuildFolderCategory.category(for: item), categories.contains(cat) {
                let target = buildFolder.appendingPathComponent(item)
                try? fileManager.removeItem(at: target)
            }
        }
        return true
    }

    public func cleanAllReclaimable(in buildFolder: URL) -> Int64 {
        guard let report = audit(buildFolder: buildFolder) else { return 0 }
        let freed = report.totalReclaimableBytes
        _ = clean(categories: Set(BuildFolderCategory.allCases), in: buildFolder)
        return freed
    }

    // MARK: - Image and Artwork Cache Audit
    public var imageCacheDirectories: [URL] {
        let appSup = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let base = appSup.appendingPathComponent("nxnative")
        return [
            base.appendingPathComponent("covers"),
            base.appendingPathComponent("artwork")
        ]
    }

    public func calculateImageCacheBytes() -> Int64 {
        var total: Int64 = 0
        for dir in imageCacheDirectories {
            total += folderSize(dir)
        }
        return total
    }

    public func cleanImageCache() -> Int64 {
        let before = calculateImageCacheBytes()
        for dir in imageCacheDirectories {
            if fileManager.fileExists(atPath: dir.path),
               let items = try? fileManager.contentsOfDirectory(atPath: dir.path) {
                for item in items {
                    let target = dir.appendingPathComponent(item)
                    try? fileManager.removeItem(at: target)
                }
            }
        }
        ArtworkStore.shared.purgeMemoryCaches()
        GameCoverService.shared.purgeMemoryCaches()
        return max(0, before - calculateImageCacheBytes())
    }

    private func folderSize(_ url: URL) -> Int64 {
        var isDir: ObjCBool = false
        guard fileManager.fileExists(atPath: url.path, isDirectory: &isDir) else { return 0 }
        if !isDir.boolValue {
            return (try? fileManager.attributesOfItem(atPath: url.path)[.size] as? Int64) ?? 0
        }

        guard let enumerator = fileManager.enumerator(at: url, includingPropertiesForKeys: [.fileSizeKey], options: [.skipsHiddenFiles]) else {
            return 0
        }

        var total: Int64 = 0
        for case let fileURL as URL in enumerator {
            if let s = try? fileURL.resourceValues(forKeys: [.fileSizeKey]).fileSize {
                total += Int64(s)
            }
        }
        return total
    }
}
