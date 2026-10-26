import Foundation
import AppKit

public struct PortedGame: Identifiable, Hashable {
    public let id = UUID()
    public let name: String
    public let url: URL
    public let platform: TargetPlatform
    public let sizeBytes: Int64
    public let modificationDate: Date
    public let bundleId: String?
    public let titleId: String?
    public let versionString: String?
    
    public init(
        name: String,
        url: URL,
        platform: TargetPlatform,
        sizeBytes: Int64,
        modificationDate: Date,
        bundleId: String? = nil,
        titleId: String? = nil,
        versionString: String? = nil
    ) {
        self.name = name
        self.url = url
        self.platform = platform
        self.sizeBytes = sizeBytes
        self.modificationDate = modificationDate
        self.bundleId = bundleId
        self.titleId = titleId
        self.versionString = versionString
    }
    
    public var uniqueKey: String {
        if let tid = titleId, !tid.isEmpty { return tid }
        if let bid = bundleId, !bid.isEmpty { return bid }
        return name
    }
    
    public var formattedSize: String {
        ByteCountFormatter.string(fromByteCount: sizeBytes, countStyle: .file)
    }
    
    public var formattedDate: String {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter.string(from: modificationDate)
    }
    
    public var isPlayableLocally: Bool {
        platform == .macApp
    }
}
