import Foundation

/// Minimum Apple iPhone / iOS A-Series silicon tier required to run a ported game smoothly.
public enum IphoneChipTier: String, CaseIterable, Identifiable, Comparable, Hashable, Sendable {
    case a15 = "A15"
    case a16 = "A16"
    case a17Pro = "A17 Pro"
    case a18 = "A18"
    case a18Pro = "A18 Pro"

    public var id: String { rawValue }

    public var rank: Int {
        switch self {
        case .a15: return 1
        case .a16: return 2
        case .a17Pro: return 3
        case .a18: return 4
        case .a18Pro: return 5
        }
    }

    public var deviceSummary: String {
        switch self {
        case .a15:
            return "iPhone 13 / 14 (A15 Bionic)"
        case .a16:
            return "iPhone 14 Pro / 15 (A16 Bionic, 6GB)"
        case .a17Pro:
            return "iPhone 15 Pro / Pro Max (A17 Pro, 8GB)"
        case .a18:
            return "iPhone 16 / 16 Plus (A18, 8GB)"
        case .a18Pro:
            return "iPhone 16 Pro / Pro Max (A18 Pro, 8GB)"
        }
    }

    public static func < (lhs: IphoneChipTier, rhs: IphoneChipTier) -> Bool {
        lhs.rank < rhs.rank
    }
}

public struct CompatibleGameItem: Identifiable, Hashable {
    public var id: String { titleId }
    public let title: String
    public let titleId: String
    public let versionTested: String
    public let sdk: String
    public let engine: String
    public let graphicsApi: String
    public let readmeRelPath: String
    public let macReq: String
    public let iosReq: String
    public let storageReq: String
    public let minRamGB: Int
    public let minChipTier: IphoneChipTier
    public let isNew: Bool
    public let addedTimestamp: TimeInterval
    
    public init(
        title: String,
        titleId: String,
        versionTested: String,
        sdk: String,
        engine: String,
        graphicsApi: String,
        readmeRelPath: String,
        macReq: String = "Mac: 8GB RAM",
        iosReq: String = "iPhone: A16+ (6GB)",
        storageReq: String = "HD: ~6 GB",
        minRamGB: Int = 8,
        minChipTier: IphoneChipTier = .a17Pro,
        isNew: Bool = false,
        addedTimestamp: TimeInterval = 0
    ) {
        self.title = title
        self.titleId = titleId
        self.versionTested = versionTested
        self.sdk = sdk
        self.engine = engine
        self.graphicsApi = graphicsApi
        self.readmeRelPath = readmeRelPath
        self.macReq = macReq
        self.iosReq = iosReq
        self.storageReq = storageReq
        self.minRamGB = minRamGB
        self.minChipTier = minChipTier
        self.isNew = isNew
        self.addedTimestamp = addedTimestamp
    }
    
    public var githubUrl: URL? {
        let base = "https://github.com/seedds/nxnative/blob/main/dm/games/"
        return URL(string: base + readmeRelPath)
    }
    
    /// Official Nintendo Switch Box Art URL (GameTDB cover art)
    public var coverArtUrl: URL? {
        nil
    }
}
