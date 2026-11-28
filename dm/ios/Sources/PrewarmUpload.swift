import CryptoKit
import Foundation
import SwiftUI

private final class PrewarmUploadDelegate: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        // Apps Script returns its receipt via a one-time GET. Keep the TSV at the original endpoint.
        guard task.originalRequest?.url?.host == "script.google.com", request.httpMethod == "GET",
              let url = request.url, url.scheme == "https", url.host == "script.googleusercontent.com" else {
            completionHandler(nil)
            return
        }
        completionHandler(URLRequest(url: url))
    }
}

enum PrewarmUpload {
    static let setting = "prewarm.upload"
    static let label = "Contribute prewarm lists automatically"
    static let detail = "Sends pso_user.tsv, the game title ID, and version to the nxnative inbox."
    private static let receipt = "prewarm.uploadedDigest"
    private static let maxBytes = 32 * 1024 * 1024
    private static let queue = DispatchQueue(label: "nxnative.prewarm-upload", qos: .utility)
    private static let session = URLSession(configuration: .ephemeral, delegate: PrewarmUploadDelegate(), delegateQueue: nil)
    private static var task: URLSessionUploadTask?
    private static let row = #/S\t[0-9a-f]{16}_b\t[0-7]|C\t[0-9a-f]{16}_b|G\t[0-9a-f]{16}\t[0-9a-f]{16}|(?:R\t|M\t(?:-|[0-9a-f]{16}_b)\t)(?:[0-9a-f]{864}|[0-9a-f]{880}|[0-9a-f]{1008}|[0-9a-f]{1024})/#

    private struct Receipt: Decodable {
        let sha256: String
    }

    private static var destination: URL? {
        guard let endpoint = Bundle.main.object(forInfoDictionaryKey: "DMPrewarmUploadURL") as? String,
              var url = URLComponents(string: endpoint), url.scheme == "https",
              url.host == "script.google.com", url.path.wholeMatch(of: #/\/macros\/s\/[A-Za-z0-9_-]+\/exec/#) != nil,
              url.user == nil, url.password == nil,
              let title = Bundle.main.object(forInfoDictionaryKey: "DMTitleID") as? String,
              title.wholeMatch(of: #/[0-9A-F]{16}/#) != nil,
              let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String,
              version.wholeMatch(of: #/[A-Za-z0-9][A-Za-z0-9 ._+()-]{0,63}/#) != nil else { return nil }
        url.queryItems = [URLQueryItem(name: "title", value: title), URLQueryItem(name: "version", value: version)]
        url.percentEncodedQuery = url.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")
        url.fragment = nil
        return url.url
    }

    static var available: Bool { destination != nil }
    static var enabled: Bool { UserDefaults.standard.object(forKey: setting) as? Bool ?? true }

    static func setEnabled(_ value: Bool) {
        UserDefaults.standard.set(value, forKey: setting)
        if value { schedule() }
        else { queue.async { task?.cancel() } }
    }

    static func schedule() {
        guard enabled, let destination, let save = getenv("DM_SAVE").map({ String(cString: $0) }) else { return }
        let file = URL(fileURLWithPath: save).deletingLastPathComponent().appendingPathComponent("pso_user.tsv")
        if let override = getenv("DM_PSO_USER"), !String(cString: override).isEmpty,
           URL(fileURLWithPath: String(cString: override)).standardizedFileURL != file.standardizedFileURL { return }
        queue.async { send(file: file, to: destination) }
    }

    static func snapshot(_ file: URL) -> Data? {
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey]
        guard let values = try? file.resourceValues(forKeys: keys), values.isRegularFile == true,
              values.isSymbolicLink != true, let size = values.fileSize, (1...maxBytes).contains(size),
              var data = try? Data(contentsOf: file), data.count <= maxBytes,
              let end = data.lastIndex(of: 10) else { return nil }
        data.removeSubrange(data.index(after: end)..<data.endIndex)
        guard data.count > 1, let text = String(data: data, encoding: .ascii),
              text.dropLast().split(separator: "\n", omittingEmptySubsequences: false).allSatisfy({
                  $0.utf8.count <= 1045 && $0.wholeMatch(of: row) != nil
              }) else { return nil }
        return data
    }

    private static func send(file: URL, to destination: URL) {
        guard task == nil, enabled, let data = snapshot(file) else { return }
        let defaults = UserDefaults.standard
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard defaults.string(forKey: receipt) != digest, enabled else { return }
        var request = URLRequest(url: destination, timeoutInterval: 60)
        request.httpMethod = "POST"
        request.setValue("text/tab-separated-values", forHTTPHeaderField: "Content-Type")
        let next = session.uploadTask(with: request, from: data) { body, response, error in
            queue.async {
                task = nil
                if error == nil, (response as? HTTPURLResponse)?.statusCode == 200,
                   let body, (try? JSONDecoder().decode(Receipt.self, from: body))?.sha256 == digest {
                    UserDefaults.standard.set(digest, forKey: receipt)
                }
            }
        }
        task = next
        next.resume()
    }
}

struct PrewarmUploadSetting: View {
    @AppStorage(PrewarmUpload.setting) private var enabled = true

    var body: some View {
        if PrewarmUpload.available {
            Toggle(isOn: Binding(get: { enabled }, set: { PrewarmUpload.setEnabled($0) })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(PrewarmUpload.label)
                    Text(PrewarmUpload.detail).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }
}

@_cdecl("dm_prewarm_upload_schedule")
public func dmPrewarmUploadSchedule() {
    PrewarmUpload.schedule()
}
