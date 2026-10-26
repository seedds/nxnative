import Foundation

public struct SelectableGame: Identifiable, Equatable {
    public let id: String
    public let name: String
    public let titleId: String?
    public var files: [GameFileItem]
    
    public init(id: String, name: String, titleId: String?, files: [GameFileItem]) {
        self.id = id
        self.name = name
        self.titleId = titleId
        self.files = files
    }
    
    public var totalSizeBytes: Int64 {
        files.reduce(0) { $0 + $1.sizeBytes }
    }
    
    public var formattedSize: String {
        ByteCountFormatter.string(fromByteCount: totalSizeBytes, countStyle: .file)
    }
    
    public var baseFile: GameFileItem? {
        files.first(where: { $0.type == .base }) ?? files.first
    }
    
    public var updateCount: Int {
        files.filter { $0.type == .update }.count
    }
    
    public var dlcCount: Int {
        files.filter { $0.type == .dlc }.count
    }
    
    public var summaryText: String {
        var parts: [String] = []
        if baseFile != nil { parts.append("Base") }
        if updateCount > 0 { parts.append("\(updateCount) Update" + (updateCount > 1 ? "s" : "")) }
        if dlcCount > 0 { parts.append("\(dlcCount) DLC" + (dlcCount > 1 ? "s" : "")) }
        return parts.joined(separator: ", ")
    }
}
