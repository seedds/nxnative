// Save data for the launchers: pack the committed save into one file, and restore one (dm/host/save_archive.c).
// Shared by the iPhone launcher and the Mac settings window, so it uses Foundation only. The host's own save stays
// where it is; an export is a copy and an import replaces it before the game starts (a launcher is the only place
// that may import, because the game's mount holds the working directory once it runs).
import Foundation
import UniformTypeIdentifiers

struct SaveDataError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

enum SaveData {
    static let typeIdentifier = "org.nxnative.save-archive"   // declared in the app's Info.plist, extension dmsave
    static var contentType: UTType { UTType(typeIdentifier) ?? UTType(filenameExtension: "dmsave") ?? .data }

    // The game this app is, as its Info.plist records it (build_ipa.sh's DM_TITLE_ID, pack_app.py's DMTitleID);
    // 0 when the bundle has none, which lets an archive of any game be imported
    static var titleID: UInt64 {
        guard let s = Bundle.main.object(forInfoDictionaryKey: "DMTitleID") as? String else { return 0 }
        return UInt64(s.trimmingCharacters(in: .whitespaces), radix: 16) ?? 0
    }

    private static var gameName: String {
        (Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String)
            ?? (Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String) ?? "Game"
    }

    // "Zelda BotW 2026-10-29 12.34.dmsave": the game and the moment, with nothing a file name cannot hold
    static func suggestedFileName() -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH.mm"
        let name = gameName.replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: "-")
        return "\(name) \(f.string(from: Date())).dmsave"
    }

    // Where an iPhone export goes without a save panel: Documents/Save Data, visible in the Files app
    // (UIFileSharingEnabled). The Mac uses its save panel, so this is the panel's starting folder there.
    static func defaultExportURL() throws -> URL {
        let fm = FileManager.default
        #if os(iOS)
        let docs = fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let dir = docs.appendingPathComponent("Save Data", isDirectory: true)
        #else
        let dir = fm.urls(for: .downloadsDirectory, in: .userDomainMask)[0]
        #endif
        if !fm.fileExists(atPath: dir.path) { try fm.createDirectory(at: dir, withIntermediateDirectories: true) }
        return dir.appendingPathComponent(suggestedFileName())
    }

    // Writes the committed save to url (the archive replaces the file atomically; a failure leaves no part file)
    static func export(to url: URL) throws {
        var buf = [CChar](repeating: 0, count: 512)
        let rc = dm_save_export(url.path, titleID, &buf, buf.count)
        if rc != 0 { throw SaveDataError(message: text(buf, "The save data could not be exported (code \(rc)).")) }
    }

    // Replaces this device's committed save with the archive at url, after checking it whole; the replaced save is
    // kept beside it as <save>.pre-import
    static func importArchive(from url: URL) throws {
        let scoped = url.startAccessingSecurityScopedResource()   // a file picked outside the app's container
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        var buf = [CChar](repeating: 0, count: 512)
        let rc = dm_save_import(url.path, titleID, &buf, buf.count)
        if rc != 0 { throw SaveDataError(message: text(buf, "The save data could not be imported (code \(rc)).")) }
    }

    private static func text(_ buf: [CChar], _ fallback: String) -> String {
        let s = buf.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
        return s.isEmpty ? fallback : s
    }
}
