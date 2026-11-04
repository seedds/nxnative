// The save library holds a directory per title. Each game's original save stays in its app data.
import Foundation
import SwiftUI
import UniformTypeIdentifiers

private struct SaveFolderError: LocalizedError {
    let errorDescription: String?
    init(_ message: String) { errorDescription = message }
}

private final class FolderAccess {
    let url: URL
#if os(iOS)
    private let scoped: Bool
#endif
    init(_ url: URL) throws {
        self.url = url
#if os(iOS)
        scoped = url.startAccessingSecurityScopedResource()
        if !scoped && !url.standardizedFileURL.path.hasPrefix(NSHomeDirectory() + "/") {
            throw SaveFolderError("Files could not grant access. Choose the folder again or use the default location.")
        }
#endif
    }
    deinit {
#if os(iOS)
        if scoped { url.stopAccessingSecurityScopedResource() }
#endif
    }
}

enum LocalSaveFolder {
    fileprivate static var title = ""
    fileprivate static var original = URL(fileURLWithPath: "/")
    private static var session: FolderAccess? // Keep the chosen iOS security scope until the game exits.
    private static var exact: String? { getenv("DM_SAVE").map { String(cString: $0) } }
    private static var externalRoot: String? { getenv("DM_SAVE_ROOT").map { String(cString: $0) } }
    static var fixed: String? { exact ?? externalRoot }
    static var fixedKey: String { exact == nil ? "DM_SAVE_ROOT" : "DM_SAVE" }
#if os(macOS)
    // All packed games on this Mac see the same library. Each title remembers the source
    // of its latest progress if another title switches the global library in between.
    private static var choiceFile: URL { original.deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("save-location.plist") }
    private static var lastFile: URL { original.deletingLastPathComponent().appendingPathComponent("last-save-location.plist") }
    private static func readPath(_ file: URL) throws -> URL? {
        let data: Data
        do { data = try Data(contentsOf: file) }
        catch let error as CocoaError where error.code == .fileReadNoSuchFile { return nil }
        guard let path = try PropertyListSerialization.propertyList(from: data, format: nil) as? String,
              path.hasPrefix("/"), !path.utf8.contains(0) else { throw SaveFolderError("The saved folder choice could not be read. Choose a folder or use the default location.") }
        return URL(fileURLWithPath: path)
    }
    private static func writePath(_ url: URL?, to file: URL) throws {
        if let url {
            try PropertyListSerialization.data(fromPropertyList: url.path, format: .binary, options: 0).write(to: file, options: .atomic)
        } else {
            do { try FileManager.default.removeItem(at: file) }
            catch let error as CocoaError where error.code == .fileNoSuchFile { }
        }
    }
    static var hasChoice: Bool { FileManager.default.fileExists(atPath: choiceFile.path) }
#else
    private static let choiceKey = "localSaves.folderBookmark"
    static var hasChoice: Bool { UserDefaults.standard.object(forKey: choiceKey) != nil }
#endif
    fileprivate static func selected() throws -> FolderAccess? {
#if os(macOS)
        return try readPath(choiceFile).map { try FolderAccess($0) }
#else
        guard hasChoice else { return nil }
        guard let data = UserDefaults.standard.data(forKey: choiceKey) else { throw SaveFolderError("The saved folder choice is unreadable. Choose it again or use the default location.") }
        var stale = false
        let url = try URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale)
        let access = try FolderAccess(url)
        if stale { UserDefaults.standard.set(try url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil), forKey: choiceKey) }
        return access
#endif
    }

    private static func prepare(_ root: URL, source: URL?) throws -> String {
        var path = [CChar](repeating: 0, count: Int(PATH_MAX)), why = [CChar](repeating: 0, count: 1024)
        guard dm_save_prepare_root(root.path, title, source?.path, &path, path.count, &why, why.count) != 0 else {
            throw SaveFolderError(String(cString: why))
        }
        return String(cString: path)
    }
    // A different game can change the Mac's shared library before this title has used
    // it. Prefer the source this title actually used. An unavailable source can still
    // reopen a populated target, but must never seed a fresh one with stale defaults.
    private static func previousSave(_ previous: FolderAccess?) throws -> URL? {
#if os(macOS)
        if let last = try readPath(lastFile) { return last.appendingPathComponent(title).appendingPathComponent("save") }
        // A pre-existing empty title directory in a newly chosen global folder has
        // no progress to copy. Its first launch must still import the original save.
        if let save = previous?.url.appendingPathComponent(title).appendingPathComponent("save"),
           FileManager.default.fileExists(atPath: save.path) { return save }
#else
        if let previous { return previous.url.appendingPathComponent(title).appendingPathComponent("save") }
#endif
        return hasChoice && previous == nil ? nil : original
    }

    static func choose(_ url: URL) throws {
        let picked = try FolderAccess(url)
#if os(iOS)
        let bookmark = try url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
        var stale = false
        _ = try URL(resolvingBookmarkData: bookmark, options: [], relativeTo: nil, bookmarkDataIsStale: &stale)
#endif
        let previous = try? selected()
        let source = try? previousSave(previous)
        try withExtendedLifetime((picked, previous)) { _ = try prepare(url, source: source) }
#if os(macOS)
        try writePath(url, to: choiceFile)
#else
        UserDefaults.standard.set(bookmark, forKey: choiceKey)
#endif
    }
    static func restore() throws {
#if os(macOS)
        try writePath(nil, to: choiceFile)
        try writePath(nil, to: lastFile)
#else
        UserDefaults.standard.removeObject(forKey: choiceKey)
#endif
    }
    fileprivate static func apply() throws {
        if exact != nil { return }
        let root: URL?
        var access: FolderAccess?
        let external = externalRoot
        if let external {
            guard external.hasPrefix("/") else { throw SaveFolderError("DM_SAVE_ROOT must name an absolute, accessible folder.") }
            let requested = URL(fileURLWithPath: external)
#if os(iOS)
            // A matching Files bookmark can grant the external path access even when an env file selects it.
            if let saved = try? selected(), saved.url.standardizedFileURL.path == requested.standardizedFileURL.path { access = saved }
#endif
            root = requested
        } else {
            access = try selected()
            root = access?.url
        }
        guard let root else {
            guard setenv("DM_SAVE", original.path, 0) == 0 else { throw SaveFolderError("Could not set the default save directory.") }
#if os(macOS)
            try writePath(nil, to: lastFile)
#endif
            return
        }
        let path: String
        do {
            let prior = external == nil ? access : try? selected()
            var source = try? previousSave(prior)
#if os(macOS)
            // For a global library newly selected by another app, this title may need
            // to import from its last library. A previously used title must still exist.
            if external == nil, let last = try readPath(lastFile), last.standardizedFileURL == root.standardizedFileURL {
                source = nil
            }
#else
            if external == nil { source = nil } // choosing the bookmark already prepared this title
#endif
            path = try withExtendedLifetime(prior) { try prepare(root, source: source) }
        }
        catch {
            if external != nil { throw SaveFolderError("DM_SAVE_ROOT could not be opened. Check the folder path and access. " + error.localizedDescription) }
            throw error
        }
#if os(macOS)
        if external == nil { try writePath(root, to: lastFile) }
#endif
        let pipelines = original.deletingLastPathComponent().appendingPathComponent("pso_user.tsv")
        guard setenv("DM_PSO_USER", pipelines.path, 0) == 0, setenv("DM_SAVE", path, 0) == 0 else {
            throw SaveFolderError("Could not set the save directory for this launch.")
        }
        session = access
    }
}

@_cdecl("dm_local_save_configure")
public func dmLocalSaveConfigure(_ title: UnsafePointer<CChar>, _ original: UnsafePointer<CChar>) {
    LocalSaveFolder.title = String(cString: title)
    LocalSaveFolder.original = URL(fileURLWithPath: String(cString: original))
}
@_cdecl("dm_local_save_apply")
public func dmLocalSaveApply(_ message: UnsafeMutablePointer<CChar>, _ size: Int) -> Int32 {
    do { try LocalSaveFolder.apply(); return 1 }
    catch { strlcpy(message, error.localizedDescription, size); return 0 }
}

// A missing volume or an expired bookmark leaves Play on the settings screen so the player
// can choose another folder. Headless launches use the same apply operation from native code.
struct LocalSavePlayButton<Label: View>: View {
    @Binding var working: Bool
    let ready: () -> Void
    @ViewBuilder var label: () -> Label
    @State private var showError = false
    @State private var message = ""

    var body: some View {
        Button(action: start, label: label)
            .disabled(working)
            .alert("Unable to open save location", isPresented: $showError) { Button("OK", role: .cancel) {} } message: { Text(verbatim: message) }
    }
    private func start() {
        working = true
        Task {
            do {
                try await Task.detached { try LocalSaveFolder.apply() }.value
                working = false
                ready()
            } catch {
                working = false
                message = error.localizedDescription
                showError = true
            }
        }
    }
}

struct LocalSaveFolderSection: View {
    @Binding var working: Bool
    @State private var picking = false
    @State private var chosen = false
    @State private var folder = ""
    @State private var error = ""
    private let fixed = LocalSaveFolder.fixed

    var body: some View {
        Section {
            LabeledContent("Local save location") {
                Text(verbatim: fixed ?? (folder.isEmpty ? (chosen ? "Unavailable" : "App default") : folder))
                    .foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            }
            if fixed == nil {
                HStack {
                    Button("Choose Folder…") { picking = true }
                        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { result in
                            do { copy(to: try result.get()) }
                            catch let failure as CocoaError where failure.code == .userCancelled { }
                            catch { self.error = error.localizedDescription }
                        }
                    if chosen { Button("Use Default") { do { try LocalSaveFolder.restore(); refresh() } catch { self.error = error.localizedDescription } } }
                }
                .buttonStyle(.borderless).disabled(working)
            }
        } footer: {
            if fixed != nil {
                Text(LocalSaveFolder.fixedKey == "DM_SAVE" ? "DM_SAVE sets the exact directory for this launch." : "DM_SAVE_ROOT sets the top-level folder for this launch.")
            } else if working {
                Text("Preparing this game’s save folder…")
            } else if !error.isEmpty {
                Text(verbatim: error).foregroundStyle(.red)
            } else {
                Text("Choose a top-level folder. Games have separate folders. Players have separate account saves and share device saves. A new game folder copies your current saves. An existing game folder reopens its own progress. Use Default reopens the app’s original saves.")
            }
        }
        .onAppear(perform: refresh)
    }
    private func refresh() {
        guard fixed == nil else { return }
        chosen = LocalSaveFolder.hasChoice
        do { folder = try LocalSaveFolder.selected()?.url.path ?? ""; error = "" }
        catch { folder = ""; self.error = error.localizedDescription }
    }
    private func copy(to url: URL) {
        working = true
        Task {
            let failure = await Task.detached { () -> String? in
                do { try LocalSaveFolder.choose(url); return nil }
                catch { return error.localizedDescription }
            }.value
            refresh()
            if let failure { error = failure }
            working = false
        }
    }
}
