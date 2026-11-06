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

private struct Switch2ControllerItem: Identifiable {
    var id: String { serial.isEmpty ? "\(player)-\(name)" : serial }
    let name: String
    let serial: String
    let player: Int
    let batteryPercent: Int
    let batteryMv: Int
    let charging: Bool
    let rssi: Int
    let connected: Bool
}

private struct PairingGuideView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var page = 0

    var body: some View {
        NavigationStack {
            VStack(spacing: 24) {
                Spacer()
                TabView(selection: $page) {
                    GuidePage(
                        icon: "gamecontroller.fill",
                        title: "Welcome",
                        description: "Connect your Nintendo Switch 2 controllers to your Apple device over Bluetooth — the Pro Controller 2, a left and right Joy-Con 2 (held alone or together as one), and the NSO GameCube pad."
                    ).tag(0)

                    GuidePage(
                        icon: "antenna.radiowaves.left.and.right",
                        title: "Pairing a Controller",
                        description: "Hold the Sync button (next to the USB-C port) until the player LEDs sweep. The controller connects automatically and remembers your device — after that, just press any button to wake it.\n\nNote: If iOS Settings > Bluetooth holds the controller, forget it there first so the app can connect."
                    ).tag(1)

                    GuidePage(
                        icon: "slider.horizontal.3",
                        title: "Make It Yours",
                        description: "Configure back buttons (GL/GR) and stick deadzones in launcher settings. Each connected controller is automatically assigned its own player slot."
                    ).tag(2)
                }
                .tabViewStyle(.page(indexDisplayMode: .always))
                .frame(maxHeight: 320)

                Spacer()

                HStack {
                    if page > 0 {
                        Button("Back") { withAnimation { page -= 1 } }
                            .buttonStyle(.bordered)
                    }
                    Spacer()
                    if page < 2 {
                        Button("Next") { withAnimation { page += 1 } }
                            .buttonStyle(.borderedProminent)
                    } else {
                        Button("Done") { dismiss() }
                            .buttonStyle(.borderedProminent)
                    }
                }
                .padding(.horizontal, 32)
                .padding(.bottom, 24)
            }
            .navigationTitle("Switch 2 Controllers")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
        }
    }
}

private struct GuidePage: View {
    let icon: String
    let title: String
    let description: String

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: icon)
                .font(.system(size: 64))
                .foregroundStyle(.tint)
                .accessibilityHidden(true)   // the page's title carries the meaning
            Text(title)
                .font(.title2.weight(.bold))
            Text(description)
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 24)
        }
    }
}

private struct ControllersSection: View {
    @State private var controllers: [Switch2ControllerItem] = []
    @State private var isScanning = false
    @State private var showGuide = false

    var body: some View {
        Section {
            if controllers.isEmpty {
                HStack(spacing: 12) {
                    Image(systemName: "gamecontroller")
                        .foregroundStyle(.secondary)
                        .accessibilityHidden(true)
                    Text("No controllers connected")
                        .foregroundStyle(.secondary)
                }
            } else {
                ForEach(controllers) { c in
                    HStack(spacing: 12) {
                        Image(systemName: "gamecontroller.fill")
                            .foregroundStyle(.tint)
                            .accessibilityHidden(true)
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(spacing: 6) {
                                Text("P\(c.player): \(c.name)")
                                    .font(.body.weight(.medium))
                                Circle().fill(c.connected ? Color.green : Color.orange)
                                    .frame(width: 8, height: 8)
                                    .accessibilityLabel(c.connected ? "Connected" : "Present, not connected")
                            }
                            if !c.serial.isEmpty {
                                Text(c.serial).font(.caption2).foregroundStyle(.secondary)
                            }
                        }
                        Spacer()
                        if c.charging {
                            Image(systemName: "bolt.fill")
                                .foregroundStyle(.yellow)
                                .accessibilityLabel("Charging")   // the battery reading next to it stays present
                        }
                        if c.batteryPercent >= 0 {
                            HStack(spacing: 4) {
                                Text("\(c.batteryPercent)%")
                                    .font(.subheadline).monospacedDigit().foregroundStyle(.secondary)
                                Image(systemName: batteryIcon(for: c.batteryPercent))
                                    .foregroundStyle(batteryColor(for: c.batteryPercent))
                                    .accessibilityHidden(true)   // the percentage next to it carries the value
                            }
                        }
                    }
                    HStack {
                        Button("Test Rumble") {
                            dm_switch2_test_rumble(Int32(c.player))
                        }
                        .font(.footnote)
                        .buttonStyle(.bordered)

                        Button("Find") {
                            dm_switch2_find(Int32(c.player))
                        }
                        .font(.footnote)
                        .buttonStyle(.bordered)

                        Spacer()

                        Button("Disconnect", role: .destructive) {
                            dm_switch2_disconnect(Int32(c.player))
                            refresh()
                        }
                        .font(.footnote)
                        .buttonStyle(.bordered)
                    }
                }
            }

            HStack {
                if isScanning {
                    ProgressView().controlSize(.small)
                        .accessibilityHidden(true)
                    Text("Scanning for controllers…")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("Stop") {
                        dm_switch2_scan_stop()
                        refresh()
                    }
                    .font(.footnote)
                    .buttonStyle(.bordered)
                } else {
                    Text("Ready to connect")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("Scan") {
                        dm_switch2_scan_start()
                        refresh()
                    }
                    .font(.footnote)
                    .buttonStyle(.bordered)
                }
            }

            Button("Pairing Guide…") { showGuide = true }
        } header: {
            Text("Nintendo Switch 2 Controllers")
        } footer: {
            Text("Hold the Sync button on your controller until player LEDs sweep to pair. If already paired, press any button to connect.")
        }
        .sheet(isPresented: $showGuide) {
            PairingGuideView()
        }
        .onAppear {
            dm_switch2_scan_start()
            refresh()
        }
        .task {
            while !Task.isCancelled {
                refresh()
                try? await Task.sleep(for: .seconds(1))
            }
        }
    }

    private func refresh() {
        var raw = [dm_switch2_controller_info](repeating: dm_switch2_controller_info(), count: 8)
        let count = dm_switch2_get_controllers(&raw, 8)
        var items: [Switch2ControllerItem] = []
        for i in 0..<Int(count) {
            let r = raw[i]
            let name = withUnsafeBytes(of: r.name) { String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self)) }
            let serial = withUnsafeBytes(of: r.serial) { String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self)) }
            items.append(Switch2ControllerItem(
                name: name,
                serial: serial,
                player: Int(r.player),
                batteryPercent: Int(r.battery_percent),
                batteryMv: Int(r.battery_mv),
                charging: r.charging != 0,
                rssi: Int(r.rssi),
                connected: r.connected
            ))
        }
        controllers = items
        isScanning = dm_switch2_is_scanning()
    }

    private func batteryIcon(for percent: Int) -> String {
        if percent >= 90 { return "battery.100" }
        if percent >= 65 { return "battery.75" }
        if percent >= 40 { return "battery.50" }
        if percent >= 15 { return "battery.25" }
        return "battery.0"
    }

    private func batteryColor(for percent: Int) -> Color {
        if percent > 20 { return .green }
        if percent > 10 { return .orange }
        return .red
    }
}

private struct LauncherView: View {
    @ObservedObject var m: LauncherModel
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
                    ControllersSection()
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
