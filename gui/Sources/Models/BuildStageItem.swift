import Foundation

public struct BuildStageItem: Identifiable, Equatable {
    public let id: Int // 1-based stage index
    public let name: String
    public var step: String
    public var isRunning: Bool
    public var isCompleted: Bool
    public var isFailed: Bool
    public var elapsedSeconds: Double?
    public var summary: String?
    
    public init(
        id: Int,
        name: String,
        step: String = "",
        isRunning: Bool = true,
        isCompleted: Bool = false,
        isFailed: Bool = false,
        elapsedSeconds: Double? = nil,
        summary: String? = nil
    ) {
        self.id = id
        self.name = name
        self.step = step
        self.isRunning = isRunning
        self.isCompleted = isCompleted
        self.isFailed = isFailed
        self.elapsedSeconds = elapsedSeconds
        self.summary = summary
    }
}
