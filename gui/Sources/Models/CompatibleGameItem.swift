import Foundation

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
    
    public init(
        title: String,
        titleId: String,
        versionTested: String,
        sdk: String,
        engine: String,
        graphicsApi: String,
        readmeRelPath: String,
        macReq: String = "Mac: 8GB RAM",
        iosReq: String = "iOS: 6GB RAM",
        storageReq: String = "HD: ~6 GB"
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
