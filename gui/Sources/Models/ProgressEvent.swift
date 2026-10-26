import Foundation

public struct ProgressEvent: Decodable {
    public let ev: String?
    public let k: Int?
    public let n: Int?
    public let name: String?
    public let step: String?
    public let done: Double?
    public let total: Double?
    public let unit: String?
    public let est: Bool?
    public let note: String?
    public let frac: Double?
    public let eta: Double?
    public let elapsed: Double?
    public let ok: Bool?
    public let summary: String?
    public let msg: String?
    public let t: Double?
}
