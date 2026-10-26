import Foundation

public enum GameFileType: String {
    case base = "Base Game"
    case update = "Update"
    case dlc = "DLC"
    case unknown = "Switch File"
    
    public var iconName: String {
        switch self {
        case .base:
            return "gamecontroller.fill"
        case .update:
            return "arrow.triangle.2.circlepath.circle.fill"
        case .dlc:
            return "sparkles.rectangle.stack.fill"
        case .unknown:
            return "doc.fill"
        }
    }
}

public struct GameFileItem: Identifiable, Hashable {
    public let id = UUID()
    public let url: URL
    public let name: String
    public let sizeBytes: Int64
    public let type: GameFileType
    public let titleId: String?
    public let versionString: String?
    
    public var formattedSize: String {
        ByteCountFormatter.string(fromByteCount: sizeBytes, countStyle: .file)
    }
}
