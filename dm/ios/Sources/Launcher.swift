// Launcher: the settings screen shown before the game starts (front_ios.m dm_show_launcher). Its settings come from the app's dm/launcher.json, which build_ipa.sh writes for this game
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
    @State private var picking = false

    var body: some View {
        Section {
            HStack {
                Text("Cloud saves")
                Spacer()
                Text(folder.isEmpty ? "Off" : (folder as NSString).lastPathComponent)
                    .foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            }
            Button(folder.isEmpty ? "Choose a folder in iCloud Drive…" : "Choose a different folder…") { picking = true }
                // The sheet sits on the button, not on the Section: SwiftUI hands a Section's modifiers to every row, so one tap presented a picker per row and they invalidated each other (LNPANDY/NX2Apple#315).
                .sheet(isPresented: $picking) {
                    DocumentPicker(types: [.folder]) { url in
                        picking = false
                        if dm_store_cloud_saves(url) {
                            note = "Saved. The game uses this folder when you press Play."
                            refresh()
                        } else {
                            note = "That folder could not be remembered — try a different one."
                        }
                    } onCancel: { picking = false }
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

/// Mods (dm/host/hle_mods.c): the ones the app was built with (dm/mods) and the player's in Documents/mods, as the host finds them. A mod of replacement files has a
/// switch; the choice goes to the host in DM_MODS_OFF and is kept in UserDefaults like the settings (launcher.DM_MODS_OFF). A mod whose code patches are part of
/// the build is always on; one with code patches the app was not built with is skipped by the host, and said so here. An env file's DM_MODS_OFF is shown, not changed
final class ModsModel: ObservableObject {
    struct Mod: Identifiable { let name: String; let kind: Int; let dir: String; var id: String { name } }
    @Published var mods: [Mod] = []
    @Published var off: Set<String>
    let fixed: Bool
    private let defaults: UserDefaults
    private static let key = "launcher.DM_MODS_OFF"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let env = ProcessInfo.processInfo.environment
        let keys = ((env["DM_ENV_FILE_KEYS"] ?? "") + " " + (env["DM_ENV_LAUNCH_KEYS"] ?? "")).split(separator: " ").map(String.init)
        fixed = keys.contains("DM_MODS_OFF")
        let saved = fixed ? env["DM_MODS_OFF"] : (env["DM_SAVED_CHOICES"] == "0" ? nil : defaults.string(forKey: Self.key))
        off = Set((saved ?? "").split(separator: "/").map(String.init))
        refresh()
    }
    func refresh() {
        let n = dm_mods_scan()
        mods = (0..<n).map { Mod(name: String(cString: dm_mod_name($0)), kind: Int(dm_mod_kind($0)), dir: String(cString: dm_mod_dir($0))) }
    }
    // The player's mods folder (main.m names it in DM_MODS after the built-in ones): Add a mod copies a picked folder into it, and a mod found there can be deleted
    let userDir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("mods")
    private func topEntry(_ m: Mod) -> URL? {   // the folder under Documents/mods that holds the mod (a download's tree may hold it deeper)
        let base = userDir.standardizedFileURL.path + "/", dir = URL(fileURLWithPath: m.dir).standardizedFileURL.path
        guard dir.hasPrefix(base), let first = dir.dropFirst(base.count).split(separator: "/").first else { return nil }
        return userDir.appendingPathComponent(String(first))
    }
    func removable(_ m: Mod) -> Bool { topEntry(m) != nil }
    func delete(_ m: Mod) {
        guard let d = topEntry(m) else { return }
        do { try FileManager.default.removeItem(at: d); fputs("launcher: mod folder \(d.lastPathComponent) deleted\n", stderr) } catch { fputs("[RED] launcher: cannot delete \(d.path): \(error)\n", stderr) }
        refresh()
    }
    // A folder picked in the Files app, copied into Documents/mods under its own name: a mod, a folder of mods or a download's tree, as the host reads them. One that adds no mod is taken out again
    func add(_ url: URL) -> String {
        let fm = FileManager.default, scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let dest = userDir.appendingPathComponent(url.lastPathComponent)
        if fm.fileExists(atPath: dest.path) { return "There is already a mod folder named \(url.lastPathComponent)." }
        let before = Set(mods.map(\.name))
        do { try fm.createDirectory(at: userDir, withIntermediateDirectories: true); try fm.copyItem(at: url, to: dest) } catch { return "The folder could not be copied: \(error.localizedDescription)" }
        refresh()
        let added = mods.filter { !before.contains($0.name) }
        if added.isEmpty { try? fm.removeItem(at: dest); refresh(); return "\(url.lastPathComponent) holds no mod for this game: a mod is a folder with romfs or exefs inside (or an Atmosphère, Ryujinx or yuzu folder for this game)." }
        fputs("launcher: mod folder \(url.lastPathComponent) added (\(added.map(\.name).joined(separator: ", ")))\n", stderr)
        return "Added: " + added.map(\.name).joined(separator: ", ") + "."
    }
    // The mods with code patches built in switch together: the app carries one copy of the modules with all their patches (hle_mods.c)
    var code: [Mod] { mods.filter { $0.kind == DM_MOD_BUILT_IN } }
    func isOn(_ m: Mod) -> Bool {
        m.kind == DM_MOD_BUILT_IN ? !code.contains { off.contains($0.name) } : m.kind == DM_MOD_FILES && !off.contains(m.name)
    }
    func set(_ m: Mod, on: Bool) {
        for x in m.kind == DM_MOD_BUILT_IN ? code : [m] { if on { off.remove(x.name) } else { off.insert(x.name) } }
    }
    // Before the game starts. Names of mods that are gone stay in the list, so a mod taken out and put back keeps its switch
    func apply(save: Bool = true) {
        if fixed { fputs("launcher: DM_MODS_OFF kept (env file or launch environment)\n", stderr); return }
        let v = off.sorted().joined(separator: "/")
        if save && ProcessInfo.processInfo.environment["DM_SAVED_CHOICES"] != "0" { if v.isEmpty { defaults.removeObject(forKey: Self.key) } else { defaults.set(v, forKey: Self.key) } }
        setenv("DM_MODS_OFF", v, 1)
        fputs("launcher: \(mods.count) mods, \(mods.filter(isOn).count) on\(v.isEmpty ? "" : " (off: \(v))")\n", stderr)
    }
}

private struct ModsSection: View {
    @ObservedObject var m: ModsModel
    @State private var picking = false
    @State private var note = ""

    var body: some View {
        Section {
            if m.mods.isEmpty { Text("No mods").foregroundStyle(.secondary) }
            ForEach(m.mods) { mod in
                Group { switch mod.kind {
                case DM_MOD_FILES:
                    Toggle(mod.name, isOn: Binding(get: { m.isOn(mod) }, set: { m.set(mod, on: $0) })).disabled(m.fixed)
                case DM_MOD_BUILT_IN:
                    VStack(alignment: .leading, spacing: 2) {
                        Toggle(mod.name, isOn: Binding(get: { m.isOn(mod) }, set: { m.set(mod, on: $0) })).disabled(m.fixed)
                        Text(m.code.count > 1 ? "Has code patches: switches on and off with the other mods that have them." : "Has code patches: off runs the game's own code.").font(.caption).foregroundStyle(.secondary)
                    }
                case DM_MOD_NEEDS_BUILD:
                    row(mod.name, "Off", note: "It has code patches, which go in only when the app is built: rebuild with --mods.")
                default:
                    row(mod.name, "Empty", note: "No files in romfs or exefs.")
                } }
                .deleteDisabled(!m.removable(mod))
            }
            .onDelete { rows in for i in rows where m.removable(m.mods[i]) { m.delete(m.mods[i]) } }   // swipe: only the mods in Documents/mods, not the built-in ones
            Button("Add a mod…") { picking = true }
                .sheet(isPresented: $picking) {   // on the button, not the Section (see Cloud saves)
                    DocumentPicker(types: [.folder]) { url in picking = false; note = m.add(url) } onCancel: { picking = false }
                }
        } header: {
            Text("Mods")
        } footer: {
            Text(note.isEmpty ? "Pick a mod's folder (with romfs inside; unzip a download in the Files app first), or put it in this app's mods folder in the Files app. Swipe left to delete one you added. Changes apply when you press Play." : note)
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)) { _ in m.refresh() }   // back from the Files app
    }

    private func row(_ name: String, _ state: String, note: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack { Text(name); Spacer(); Text(state).foregroundStyle(.secondary) }
            Text(note).font(.caption).foregroundStyle(.secondary)
        }
    }
}

/// UIDocumentPickerViewController for a folder, in the two callbacks a SwiftUI sheet wants.
private struct DocumentPicker: UIViewControllerRepresentable {
    var types: [UTType] = [.folder]
    let onPick: (URL) -> Void
    let onCancel: () -> Void

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        // .open (not .import): the folder stays where it is and the bookmark keeps pointing at it.
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: types, asCopy: false)
        picker.allowsMultipleSelection = false
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ vc: UIDocumentPickerViewController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(onPick: onPick, onCancel: onCancel) }

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let onPick: (URL) -> Void
        let onCancel: () -> Void
        init(onPick: @escaping (URL) -> Void, onCancel: @escaping () -> Void) {
            self.onPick = onPick; self.onCancel = onCancel
        }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            if let url = urls.first { onPick(url) } else { onCancel() }
        }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { onCancel() }
    }
}

private struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    @ObservedObject var mods: ModsModel
    let play: () -> Void
    @State private var memory = os_proc_available_memory()

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
                    ModsSection(m: mods)
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
        .task {
            while !Task.isCancelled {
                memory = os_proc_available_memory()
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    @ViewBuilder var playButton: some View {
        let button = Button(action: play) { Text("Play").font(.title3.weight(.semibold)).frame(maxWidth: 220) }.controlSize(.large)
        if #available(iOS 26.0, *) { button.buttonStyle(.glassProminent) } else { button.buttonStyle(.borderedProminent) }
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
    guard let url = Bundle.main.resourceURL?.appendingPathComponent("dm/launcher.json"), let data = try? Data(contentsOf: url) else { return false }
    guard let file = try? JSONDecoder().decode(SettingsFile.self, from: data), !file.settings.isEmpty else { fputs("[RED] launcher: dm/launcher.json cannot be read; starting the game\n", stderr); return false }
    if ProcessInfo.processInfo.environment["DM_LAUNCHER"] == "0" { fputs("launcher: off (DM_LAUNCHER=0)\n", stderr); LauncherModel(file.settings).applySaved(); ModsModel().apply(save: false); return false }
    let parent = Unmanaged<UIViewController>.fromOpaque(viewController).takeUnretainedValue()
    let model = LauncherModel(file.settings), mods = ModsModel()
    var host: UIViewController?
    let view = LauncherView(m: model, mods: mods) {
        guard let h = host else { return }
        host = nil
        model.apply()
        mods.apply()
        h.dismiss(animated: false) { done() }
    }
    let h = UIHostingController(rootView: view)
    h.modalPresentationStyle = .fullScreen
    host = h
    parent.present(h, animated: false)
    fputs("launcher: \(file.settings.count) settings shown\n", stderr)
    return true
}
