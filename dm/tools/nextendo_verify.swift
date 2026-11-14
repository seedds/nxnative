// From RobiiAragon/NX-Port-Studio dm/tools/nextendo_verify.swift @ bd13a49b (MIT). Build: xcrun swiftc -parse-as-library nextendo_verify.swift (dm/tools/nextendo_patches.py does)
// Original offline signature boundary for public provider build documents.
import Foundation
import CryptoKit

@main struct VerifyNextendoDocument {
    static func main() {
        do {
            guard (3...4).contains(CommandLine.arguments.count) else { throw Failure.invalid }
            let games = CommandLine.arguments.count == 4 && CommandLine.arguments[3] == "games"
            if CommandLine.arguments.count == 4 && !games { throw Failure.invalid }
            let input = URL(fileURLWithPath: CommandLine.arguments[1])
            guard let size = try input.resourceValues(forKeys: [.fileSizeKey]).fileSize, (1...2_097_152).contains(size)
            else { throw Failure.invalid }
            let envelope = try Data(contentsOf: input)
            guard let outer = try JSONSerialization.jsonObject(with: envelope) as? [String: Any],
                  outer["key_id"] as? String == (games ? "catalog-key-1" : "npln-patches-key-1"),
                  let encoded = outer["payload"] as? String, let payload = Data(base64Encoded: encoded), payload.base64EncodedString() == encoded,
                  let signature = outer["signature"] as? String, let bytes = Data(base64Encoded: signature), bytes.count == 64,
                  bytes.base64EncodedString() == signature else { throw Failure.invalid }
            let key = try Curve25519.Signing.PublicKey(rawRepresentation: Data(base64Encoded: games ? "xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4=" : "LAI+Pi+GZtj3p4DpNM5ORBraw1cbZCVSMFgUmAVWNeA=")!)
            guard key.isValidSignature(bytes, for: payload) else { throw Failure.invalid }
            guard let object = try JSONSerialization.jsonObject(with: payload) as? [String: Any],
                  object["document_type"] as? String == (games ? "game_catalog" : "npln_patch_catalog"),
                  let schema = object["schema_version"] as? NSNumber, CFGetTypeID(schema) != CFBooleanGetTypeID(), schema == 1,
                  let revision = object["revision"] as? NSNumber, CFGetTypeID(revision) != CFBooleanGetTypeID(), Int64(revision.stringValue).map({ $0 > 0 }) == true,
                  let issued = date(object["issued_at"]), let expires = date(object["expires_at"]),
                  issued <= Date().addingTimeInterval(300), expires > Date(), expires > issued,
                  expires.timeIntervalSince(issued) <= 86_400 else { throw Failure.invalid }
            try payload.write(to: URL(fileURLWithPath: CommandLine.arguments[2]), options: .atomic)
        } catch { fputs("Nextendo's signed patch document could not be verified.\n", stderr); exit(1) }
    }
    enum Failure: Error { case invalid }
    static func date(_ value: Any?) -> Date? {
        guard let text = value as? String, text.count <= 40, text.hasSuffix("Z") else { return nil }
        let parser = ISO8601DateFormatter(); parser.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return parser.date(from: text)
    }
}
