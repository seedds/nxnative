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

/// The birthday a game reads from the Nintendo Account info in Nextendo mode (hle_account.c, DM_NEXTENDO_BIRTHDAY), when the Nextendo account gives none (the host
/// takes the account's first); iOS does not give apps the owner's. Kept under its own key as YYYY-MM-DD; 2000-01-01 (what Ryujinx-Nextendo and Mush8 send) until the player picks one
private let nextendoBirthdayKey = "nextendo.birthday"
private let nextendoBirthdayDefault = "2000-01-01"
private let nextendoBirthdayFormat: DateFormatter = {
    let f = DateFormatter(); f.locale = Locale(identifier: "en_US_POSIX"); f.calendar = Calendar(identifier: .gregorian)
    f.timeZone = TimeZone(identifier: "UTC"); f.dateFormat = "yyyy-MM-dd"; return f
}()

/// Nextendo: the account a game in Nextendo mode plays online with (NextendoSignIn.m). Only in an app built with --nextendo.
private struct NextendoSection: View {
    @State private var account: String? = nil
    @State private var note = ""
    @State private var busy = false
    @AppStorage(nextendoBirthdayKey) private var birthday = nextendoBirthdayDefault
    private var birthdayDate: Binding<Date> {
        Binding(get: { nextendoBirthdayFormat.date(from: birthday) ?? nextendoBirthdayFormat.date(from: nextendoBirthdayDefault)! },
                set: { birthday = nextendoBirthdayFormat.string(from: $0) })
    }

    var body: some View {
        Section {
            HStack {
                Text("Nextendo account")
                Spacer()
                Text(busy ? "Checking…" : account ?? "Not signed in").foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            }
            if dm_nextendo_signed_in() {
                DatePicker("Birthday, if Nextendo has none", selection: birthdayDate, in: ...Date(), displayedComponents: .date)
                Button("Sign out", role: .destructive) { dm_nextendo_sign_out(); account = nil; note = "" }.disabled(busy)
            } else {
                Button("Sign in…") {
                    busy = true; note = ""
                    dm_nextendo_sign_in { a, e in busy = false; account = a; note = e ?? "" }
                }.disabled(busy)
            }
        } header: { Text("Online") } footer: {
            Text(note.isEmpty ? "Online play goes through Nextendo, a community service. Set Network to Nextendo to use it. Nextendo asks for a verified e-mail address and a Discord link before online play." : note)
        }
        .onAppear {
            guard dm_nextendo_signed_in() else { return }
            busy = true
            dm_nextendo_check { a, e in busy = false; account = a; note = e ?? "" }
        }
    }
}

/// The name other players see (DM_NICK; hle_account.c GetNickname). Empty: the Nextendo username in Nextendo mode, else Player. At most 10 characters, a Switch
/// nickname's limit, which game menus are sized for. Kept under its own key and put in the environment at Play while Network is not offline
private let playerNameKey = "player.name"
private let playerNameMax = 10
private struct PlayerNameSection: View {
    @Binding var name: String
    let fixed: Bool   // an env file or the launch set DM_NICK: shown, not changed

    var body: some View {
        Section {
            TextField("Use my Nextendo name", text: $name)
                .textInputAutocapitalization(.never).autocorrectionDisabled().disabled(fixed)
                .onChange(of: name) { _, typed in if typed.count > playerNameMax { name = String(typed.prefix(playerNameMax)) } }
        } header: { Text("Player name") } footer: {
            Text(fixed ? "Set by an env file or at launch." : "The name other players see, up to 10 characters. Leave it empty to use your Nextendo username (Player when not signed in).")
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
    let play: () -> Void
    @State private var memory = os_proc_available_memory()
    @State private var checking = false
    @State private var check = 0   // which Play the running check is for: one the player went on from without it is dropped
    @State private var report: SaveSyncReport?
    @State private var asking: SaveSyncReport.Step?

    @AppStorage(playerNameKey) private var playerName = ""
    private var nameFixed: Bool { m.fixed.contains("DM_NICK") }
    private var nameShown: Bool { ["lan", "ldn", "nextendo", "auto"].contains(m.values["DM_NETWORK"] ?? "") }

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
                    if nameShown {
                        PlayerNameSection(name: nameFixed ? .constant(getenv("DM_NICK").map { String(cString: $0) } ?? "") : $playerName, fixed: nameFixed)
                    }
                    Section {
                        Button("Reset to tested defaults") { m.reset() }
                            .disabled(!m.changed)
                    } footer: {
                        Text("Brings back all settings this game was tested with.")
                    }
                    if dm_nextendo_built() { NextendoSection() }
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
        .confirmationDialog(SaveSyncReport.chooseTitle, isPresented: isAsking(.choose), titleVisibility: .visible, presenting: report) { r in
            Button(r.localLabel(here: "iPhone")) { go(r.localChoice) }
            Button(r.cloudLabel) { go(r.cloudChoice) }
            Button("Cancel", role: .cancel) {}
        } message: { r in Text(r.chooseMessage(here: "iPhone")) }
        .confirmationDialog(SaveSyncReport.unreadableTitle, isPresented: isAsking(.unreadable), titleVisibility: .visible, presenting: report) { r in
            Button("Play This iPhone's Save") { go("offline") }   // the mount does not wait for the cloud again
            Button("Try Again") { DispatchQueue.main.async { start() } }
            Button("Cancel", role: .cancel) {}
        } message: { r in Text(r.unreadableMessage(here: "iPhone")) }
        .task {
            while !Task.isCancelled {
                memory = os_proc_available_memory()
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    @ViewBuilder var playButton: some View {
        let button = Button(action: start) { Text("Play").font(.title3.weight(.semibold)).frame(maxWidth: 220) }.controlSize(.large).disabled(checking)
        if #available(iOS 26.0, *) { button.buttonStyle(.glassProminent) } else { button.buttonStyle(.borderedProminent) }
        if checking {
            VStack(spacing: 6) {
                HStack(spacing: 6) { ProgressView(); Text("Checking the cloud save…").font(.callout).foregroundStyle(.secondary) }
                Button("Play Without It") { checking = false; go("offline") }.font(.callout)
            }
        }
    }

    private func isAsking(_ step: SaveSyncReport.Step) -> Binding<Bool> {
        Binding(get: { asking == step }, set: { if !$0 && asking == step { asking = nil } })
    }

    /// Play, once the cloud's save is compared with this iPhone's (save_sync_report, SaveSyncReport): a save that changed on both sides is the player's pick, not the
    /// newer file's, and a cloud save that did not come is said so before the game starts without it.
    private func start() {
        check += 1; checking = true; asking = nil
        let mine = check
        dm_cloud_saves_env()   // DM_SAVE_SYNC from the picked folder, as the game's mount will read it (front_ios.m calls it again at the start)
        DispatchQueue.global(qos: .userInitiated).async {
            let r = Self.compare()
            DispatchQueue.main.async {
                guard checking, check == mine else { return }   // the player played without it
                checking = false
                switch SaveSyncReport.step(r) {
                case .play(let choice): go(choice)
                case let step: report = r; asking = step
                }
            }
        }
    }

    /// Blocks while the cloud's file downloads (SaveSyncReport.waitMs at most). nil: no report (the host then decides as it always did)
    private static func compare() -> SaveSyncReport? {
        // getenv, not ProcessInfo.environment: main.m sets DM_SAVE and applies env.txt after that snapshot
        guard let save = getenv("DM_SAVE") else { fputs("launcher: save sync: no DM_SAVE\n", stderr); return nil }
        var buf = [CChar](repeating: 0, count: 1024)
        guard save_sync_report(save, dm_save_title(), nil, SaveSyncReport.waitMs, &buf, buf.count) == 0 else {
            fputs("launcher: save sync: no report on \(String(cString: save))\n", stderr)
            return nil
        }
        let line = String(cString: buf)
        fputs("launcher: save sync: \(line)\n", stderr)
        return SaveSyncReport(line: line)
    }

    private func go(_ choice: String?) {
        asking = nil
        if let choice { setenv("DM_SAVE_SYNC_CHOICE", choice, 1); fputs("launcher: DM_SAVE_SYNC_CHOICE=\(choice)\n", stderr) }
        applyNextendoProfile()
        applyPlayerName()
        play()
    }

    /// The typed player name (PlayerNameSection), while Network is not offline; what an env file or the launch set stays, and an empty field leaves the game's default
    private func applyPlayerName() {
        let typed = playerName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !nameFixed, nameShown, !typed.isEmpty else { return }
        setenv("DM_NICK", String(typed.prefix(playerNameMax)), 1); fputs("launcher: DM_NICK set\n", stderr)
    }

    /// The birthday picked under the Nextendo account and the device's region, for the Nintendo Account info a game reads online (hle_account.c: the Nextendo
    /// account's own country comes first). What an env file or the launch set stays
    private func applyNextendoProfile() {
        guard dm_nextendo_built() else { return }
        if getenv("DM_NEXTENDO_BIRTHDAY") == nil, let b = UserDefaults.standard.string(forKey: nextendoBirthdayKey), nextendoBirthdayFormat.date(from: b) != nil {
            setenv("DM_NEXTENDO_BIRTHDAY", b, 1); fputs("launcher: DM_NEXTENDO_BIRTHDAY set\n", stderr)
        }
        if getenv("DM_NEXTENDO_COUNTRY") == nil, let region = Locale.current.region?.identifier, region.count == 2, region.allSatisfy({ $0.isASCII && $0.isUppercase }) {
            setenv("DM_NEXTENDO_COUNTRY", region, 1); fputs("launcher: DM_NEXTENDO_COUNTRY=\(region)\n", stderr)
        }
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
