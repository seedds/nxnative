import Foundation

public struct CompatibleGameItem: Identifiable, Hashable {
    public let id = UUID()
    public let title: String
    public let titleId: String
    public let versionTested: String
    public let sdk: String
    public let engine: String
    public let graphicsApi: String
    public let readmeRelPath: String
    
    public init(
        title: String,
        titleId: String,
        versionTested: String,
        sdk: String,
        engine: String,
        graphicsApi: String,
        readmeRelPath: String
    ) {
        self.title = title
        self.titleId = titleId
        self.versionTested = versionTested
        self.sdk = sdk
        self.engine = engine
        self.graphicsApi = graphicsApi
        self.readmeRelPath = readmeRelPath
    }
    
    public var githubUrl: URL? {
        let base = "https://github.com/seedds/nxnative/blob/main/dm/games/"
        return URL(string: base + readmeRelPath)
    }
    
    /// Official Nintendo Switch Box Art / Icon URL
    public var coverArtUrl: URL? {
        let clean = titleId.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        guard clean.count >= 16 else { return nil }
        return URL(string: "https://tinfoil.media/ti/\(clean)/256/256/")
    }
}
