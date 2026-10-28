import Foundation

public struct UpdateInfo {
    public let hasUpdate: Bool
    public let localCommit: String
    public let remoteCommit: String
    public let commitMessages: [String]
    public let checkDate: Date
    public let repoURL: String

    public init(
        hasUpdate: Bool,
        localCommit: String,
        remoteCommit: String,
        commitMessages: [String],
        checkDate: Date,
        repoURL: String
    ) {
        self.hasUpdate = hasUpdate
        self.localCommit = localCommit
        self.remoteCommit = remoteCommit
        self.commitMessages = commitMessages
        self.checkDate = checkDate
        self.repoURL = repoURL
    }
}

public protocol UpdateCheckerDelegate: AnyObject {
    func updateCheckerDidFindUpdate(_ info: UpdateInfo)
    func updateCheckerDidProduceLog(_ message: String)
}
