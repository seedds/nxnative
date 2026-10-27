import Foundation

public enum LogLevel: String {
    case info
    case step
    case success
    case warning
    case error
    
    public var sfSymbol: String {
        switch self {
        case .info: return "info.circle.fill"
        case .step: return "gearshape.fill"
        case .success: return "checkmark.circle.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .error: return "xmark.circle.fill"
        }
    }
}

public struct LogEntry: Identifiable {
    public let id = UUID()
    public let timestamp: Date
    public let level: LogLevel
    public let message: String
    
    public init(level: LogLevel = .info, message: String) {
        self.timestamp = Date()
        self.level = level
        self.message = message
    }
    
    public var formattedTimestamp: String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter.string(from: timestamp)
    }
}
