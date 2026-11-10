// Launcher: the settings screen shown before the game starts (front_ios.m dm_show_launcher). Its settings come from the app's dm/launcher.json (an app of several programs: dm/p<N>/launcher.json), which build_ipa.sh writes for this game
// from dm/games/settings.tsv (dm/games/settings.py): each sets an environment variable the host reads when the game starts. A variable an env file set (Documents/env.txt, the bundle's
// dm/env.txt; main.m lists them in DM_ENV_FILE_KEYS) is shown but kept. All settings share one screen, grouped by section (rows: LauncherRow.swift). Choices that differ from the game's default are kept
// in UserDefaults; Reset to tested defaults shows the defaults, and Play then forgets them. DM_LAUNCHER=0 in Documents/env.txt starts the game at once with the saved choices; DM_SAVED_CHOICES=0 ignores them (and keeps them)
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import os

private func info(_ key: String) -> Any? { Bundle.main.object(forInfoDictionaryKey: key) }


/// Cloud saves: an iCloud Drive folder shared with the Mac app (chosen in its settings window's
/// Cloud saves too). The host brings the save in from there when the game starts and
/// sends it there after each save (dm/host/save_sync.h); the folder is a bookmark (CloudSaves.m).
private struct CloudSavesSection: View {
    @State private var folder: String = ""
    @State private var note: String = ""

    var body: some View {
        Section {
            HStack {
                Text("Cloud saves")
                Spacer()
                Text(folder.isEmpty ? "Off" : (folder as NSString).lastPathComponent)
                    .foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            }
            Button(folder.isEmpty ? "Choose a folder in iCloud Drive…" : "Choose a different folder…") {
                FolderPicker.shared.present { url in
                    if dm_store_cloud_saves(url) {
                        note = "Saved. The game uses this folder when you press Play."
                        refresh()
                    } else {
                        note = "That folder could not be remembered — try a different one."
                    }
                }
            }
            if !folder.isEmpty {
                Button("Turn off cloud saves", role: .destructive) {
                    dm_clear_cloud_saves(); refresh(); note = ""
                }
            }
        } footer: {
            Text(note.isEmpty
                 ? "Choose a folder in iCloud Drive, and the same one in the Mac app's Cloud saves. The save comes in when the game starts and goes out after each save; a save that is replaced is kept in save-backups."
                 : note)
        }
        .onAppear(perform: refresh)
    }

    private func refresh() {
        var stale: ObjCBool = false
        folder = dm_cloud_saves_path(&stale) ?? ""
        if stale.boolValue && folder.isEmpty {
            note = "The folder you chose before is no longer readable (the app was signed differently). Please choose it again."
        }
    }
}

/// UIDocumentPickerViewController for a folder, presented the UIKit way.
///
/// The picker is a UINavigationController: embedding it in a SwiftUI `.sheet` through
/// UIViewControllerRepresentable hands it to the sheet's own container, and its internal navigation then draws
/// (an empty Recents) but stops taking touches - the Cloud saves button opened a screen you had to force quit.
/// Presenting it on the topmost view controller keeps it whole.
final class FolderPicker: NSObject, UIDocumentPickerDelegate {
    static let shared = FolderPicker()
    private var onPick: ((URL) -> Void)?

    private func log(_ s: String) { FileHandle.standardError.write(Data("launcher: \(s)\n".utf8)) }

    func present(onPick: @escaping (URL) -> Void) {
        guard let top = FolderPicker.topmost() else {
            log("no view controller to present the folder picker on")
            return
        }
        self.onPick = onPick
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder], asCopy: false)
        picker.allowsMultipleSelection = false
        picker.delegate = self
        // The picker's chrome draws before its remote view service attaches, so taps land nowhere for a moment
        // and the first one looks ignored. Nothing to do about it but leave the presentation alone.
        top.present(picker, animated: true)
        log("folder picker presented on \(type(of: top))")
    }

    private static func topmost() -> UIViewController? {
        let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .first { $0.activationState == .foregroundActive } ?? UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        var vc = scene?.windows.first { $0.isKeyWindow }?.rootViewController ?? scene?.windows.first?.rootViewController
        while let p = vc?.presentedViewController { vc = p }
        return vc
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        let cb = onPick; onPick = nil
        if let url = urls.first { cb?(url) }
    }
    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        onPick = nil
    }
}

/// A save that changed on both this iPhone and the cloud: what the settings screen asks about before the game starts
/// (dm/host/save_sync.h save_sync_report; the host's own answer is DM_SAVE_SYNC_CHOICE, docs/frontends.md).
/// Without the prompt the host silently keeps the newer one, which a brand-new file written after a real save wins.
struct SaveSyncConflict {
    var localHash = "", cloudHash = "", cloudDevice = "", cloudTitle = ""
    var localSaved = Date(timeIntervalSince1970: 0), cloudSaved = Date(timeIntervalSince1970: 0)
    var localFiles = 0, cloudFiles = 0
    var cloudRead = false, conflict = false

    /// The two saves as the host compares them; nil when this app has no cloud folder or cannot read its own save. Blocks while the cloud's
    /// file downloads (10 s at most): call it off the main thread.
    static func report() -> SaveSyncConflict? {
        // getenv, not ProcessInfo.environment: that is a snapshot from before main.m applies the app's env.txt (DM_SAVE, DM_SAVE_SYNC)
        guard let save = getenv("DM_SAVE") else { fputs("launcher: save sync: no DM_SAVE\n", stderr); return nil }
        var stale: ObjCBool = false
        guard let folder = dm_cloud_saves_path(&stale) else { fputs("launcher: save sync: no cloud saves folder\n", stderr); return nil }
        let id = String(cString: dm_save_sync_title_id())
        guard !id.isEmpty else { fputs("launcher: save sync: no title ID in this build\n", stderr); return nil }
        let cloud = (folder as NSString).appendingPathComponent((id as NSString).appendingPathComponent("save.dmsync"))
        var buf = [CChar](repeating: 0, count: 1024)
        guard save_sync_report(save, cloud, id, &buf, buf.count) == 0 else {
            fputs("launcher: save sync: cannot report on \(String(cString: save)) and \(cloud)\n", stderr)
            return nil
        }
        fputs("launcher: save sync: \(String(cString: buf))", stderr)
        var f: [String: String] = [:]
        for pair in String(cString: buf).split(separator: " ") {
            let kv = pair.split(separator: "=", maxSplits: 1)
            // the report ends its line with \n: an untrimmed value makes "1" into "1\n" and every flag reads false
            if kv.count == 2 { f[String(kv[0])] = String(kv[1].filter { !$0.isWhitespace }) }
        }
        guard f["sametitle"] != "0" else { return nil }   // the cloud's file is another game's save
        var c = SaveSyncConflict()
        c.localHash = f["local"] == "-" ? "" : (f["local"] ?? "")
        c.cloudHash = f["chash"] == "-" ? "" : (f["chash"] ?? "")
        c.cloudDevice = f["cdevice"] == "-" ? "" : (f["cdevice"] ?? "")
        c.localSaved = Date(timeIntervalSince1970: Double(f["saved"] ?? "0") ?? 0)
        c.cloudSaved = Date(timeIntervalSince1970: Double(f["csaved"] ?? "0") ?? 0)
        c.localFiles = Int(f["files"] ?? "0") ?? 0
        c.cloudFiles = Int(f["cfiles"] ?? "0") ?? 0
        c.cloudRead = f["cloud"] == "1"
        c.conflict = f["conflict"] == "1"
        return c
    }

    private static func when(_ d: Date) -> String {
        let f = DateFormatter()
        f.dateStyle = .short
        f.timeStyle = .short
        return f.string(from: d)
    }

    var localLabel: String { localFiles == 0 ? "This iPhone has no save — use the cloud's" : "This iPhone — saved \(Self.when(localSaved))" }
    var cloudLabel: String {
        guard cloudRead, cloudFiles > 0 else { return "iCloud has no save — use this iPhone's" }
        return cloudDevice.isEmpty ? "iCloud — saved \(Self.when(cloudSaved))" : "iCloud from \(cloudDevice) — saved \(Self.when(cloudSaved))"
    }

    var message: String {
        "Played on both without syncing in between, so neither replaces the other until you pick one. The one you do not pick is kept in the save's backups folder."
    }
}

private struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    let play: () -> Void
    @State private var memory = os_proc_available_memory()
    @State private var conflict: SaveSyncConflict?
    @State private var checking = false

    var body: some View {
        GeometryReader { geometry in
            HStack(spacing: 0) {
                VStack(spacing: 16) {
                    if let icon = appIcon() {
                        Image(uiImage: icon).resizable().frame(width: 96, height: 96)
                            .clipShape(RoundedRectangle(cornerRadius: 22))
                            .accessibilityHidden(true)
                    }
                    Text((info("CFBundleDisplayName") as? String) ?? "Game")
                        .font(.title.weight(.bold)).multilineTextAlignment(.center)
                    Text("Version \((info("CFBundleShortVersionString") as? String) ?? "")")
                        .font(.callout).foregroundStyle(.secondary)
                    playButton
                }
                .padding(16)
                .frame(width: geometry.size.width / 3, height: geometry.size.height)
                List {
                    ForEach(m.sections, id: \.self) { section in
                        let rows = m.rows(in: section)
                        if !rows.isEmpty {
                            Section(section) {
                                ForEach(rows) { SettingRow(s: $0, m: m) }
                            }
                        }
                    }
                    Section {
                        Button("Reset to tested defaults") { m.reset() }
                            .disabled(!m.changed)
                    } footer: {
                        Text("Brings back all settings this game was tested with.")
                    }
                    CloudSavesSection()
                    Section {
                        HStack {
                            Text("Memory available")
                            Spacer()
                            Text(ByteCountFormatter.string(fromByteCount: Int64(memory), countStyle: .memory)).monospacedDigit().foregroundStyle(.secondary)
                        }
                    }
                }
                .listStyle(.insetGrouped)
                .contentMargins(.horizontal, 12, for: .scrollContent)
                .frame(width: geometry.size.width * 2 / 3)
            }
        }
        .preferredColorScheme(.dark)
        .confirmationDialog("Both saves changed", isPresented: Binding(get: { conflict != nil }, set: { if !$0 { conflict = nil } }),
                            titleVisibility: .visible, presenting: conflict) { c in
            Button(c.localLabel) { choose(local: true, c) }
            Button(c.cloudLabel) { choose(local: false, c) }
            Button("Cancel", role: .cancel) { conflict = nil }
        } message: { c in Text(c.message) }
        .task {
            while !Task.isCancelled {
                memory = os_proc_available_memory()
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    @ViewBuilder var playButton: some View {
        VStack(spacing: 6) {
            let button = Button(action: start) { Text("Play").font(.title3.weight(.semibold)).frame(maxWidth: 220) }
                .controlSize(.large).disabled(checking)
            if #available(iOS 26.0, *) { button.buttonStyle(.glassProminent) } else { button.buttonStyle(.borderedProminent) }
            if checking && conflict == nil {
                Text("Checking the cloud save…").font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    /// Play, after the cloud's save is compared with this device's (dm/host/save_sync.h save_sync_report): a save that changed on both sides is
    /// the player's call, not the newer file's. Everything else passes `ask`, which leaves the host's own decision in place.
    private func start() {
        guard !checking else { return }
        checking = true
        fputs("launcher: play pressed; comparing the saves\n", stderr)
        DispatchQueue.global(qos: .userInitiated).async {
            let c = SaveSyncConflict.report()
            DispatchQueue.main.async {
                checking = false
                if let c, c.conflict {
                    fputs("launcher: both saves changed; showing the choice\n", stderr)
                    conflict = c
                    return
                }
                // A report that could not be read leaves DM_SAVE_SYNC_CHOICE alone: `ask` with nothing compared would hold the cloud's save back
                if c == nil { fputs("launcher: no report; playing as before\n", stderr); play(); return }
                fputs("launcher: no conflict; playing\n", stderr)
                setenv("DM_SAVE_SYNC_CHOICE", "ask", 1)
                play()
            }
        }
    }

    private func choose(local useLocal: Bool, _ c: SaveSyncConflict) {
        let choice = useLocal ? "local:" + c.localHash : "cloud:" + c.cloudHash
        setenv("DM_SAVE_SYNC_CHOICE", choice, 1)
        fputs("launcher: the player chose \(choice)\n", stderr)
        conflict = nil
        play()
    }

    private func appIcon() -> UIImage? {
        if let i = UIImage(named: "AppIcon") { return i }
        guard let icons = info("CFBundleIcons") as? [String: Any], let primary = icons["CFBundlePrimaryIcon"] as? [String: Any],
              let files = primary["CFBundleIconFiles"] as? [String], let last = files.last else { return nil }
        return UIImage(named: last)
    }
}

// Shows the launcher over the root view controller and calls done once the settings are in the environment; false (done is not called) when there is no launcher
@_cdecl("dm_show_launcher")
public func dmShowLauncher(_ viewController: UnsafeMutableRawPointer, _ done: @convention(c) () -> Void) -> Bool {
    // a game of several programs (main.m): the running program's settings, in its dm/p<N>
    let dir = getenv("DM_APP_DM").map { URL(fileURLWithPath: String(cString: $0)) } ?? Bundle.main.resourceURL?.appendingPathComponent("dm")
    guard let url = dir?.appendingPathComponent("launcher.json"), let data = try? Data(contentsOf: url) else { return false }
    guard let file = try? JSONDecoder().decode(SettingsFile.self, from: data), !file.settings.isEmpty else { fputs("[RED] launcher: dm/launcher.json cannot be read; starting the game\n", stderr); return false }
    let model = LauncherModel(file.settings)
    gameMenuModel = model   // the in-game menu shows and saves the same choices (GameMenu.swift)
    if ProcessInfo.processInfo.environment["DM_LAUNCHER"] == "0" { fputs("launcher: off (DM_LAUNCHER=0)\n", stderr); model.applySaved(); return false }
    let parent = Unmanaged<UIViewController>.fromOpaque(viewController).takeUnretainedValue()
    var host: UIViewController?
    let view = LauncherView(m: model) {
        guard let h = host else { return }
        host = nil
        model.apply()
        h.dismiss(animated: false) { done() }
    }
    let h = DmHostingController(rootView: view)
    h.modalPresentationStyle = .fullScreen
    host = h
    parent.present(h, animated: false)
    fputs("launcher: \(file.settings.count) settings shown\n", stderr)
    return true
}
