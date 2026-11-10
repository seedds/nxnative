// Mac app launcher: the settings window shown before the game starts, in the launcher's own process (launcher.c calls dm_mac_launcher before it starts the host). Its settings come from the
// app's Resources/dm/launcher.json (dm/games/settings.py with SETTINGS_PLATFORM=mac); the model and the rows are the iOS launcher's (dm/ios/Sources/LauncherSettings.swift, LauncherRow.swift),
// and so is the store: launcher.<VAR> in the app's preferences, which launcher.c reads when the window is not shown (docs/frontends.md, Saved choices). Basic settings (level basic) come
// first, the others under Advanced settings, each in the list's sections. Play puts the settings into this process's environment, which the host inherits; Quit and closing the window end the
// app without the game. "Show this window at launch" off: no window unless Option is held at launch, and launcher.c applies the saved choices. The window never takes focus by itself: an app
// opened in the background (open -g) keeps its window behind until the player switches to it
import AppKit
import QuartzCore
import SwiftUI

private func info(_ key: String) -> String? { Bundle.main.object(forInfoDictionaryKey: key) as? String }

/// The host's report of the two saves (dm/host/save_sync.c). This launcher links save_sync.o but imports no C header (dm/mac/pack_app.py builds it
/// with swiftc and no bridging header), so the symbol is declared here rather than imported.
@_silgen_name("save_sync_report")
private func save_sync_report(_ save: UnsafePointer<CChar>, _ cloud: UnsafePointer<CChar>, _ title: UnsafePointer<CChar>,
                              _ out: UnsafeMutablePointer<CChar>, _ n: Int) -> Int32

/// A save that changed on both this Mac and the cloud: what the settings window asks before the game starts
/// (dm/host/save_sync.h save_sync_report; the answer is DM_SAVE_SYNC_CHOICE, docs/frontends.md). The iOS launcher has its own copy of this
/// (dm/ios/Sources/Launcher.swift): the two front ends are separate processes and share no Swift module.
struct SaveSyncConflict {
    var localHash = "", cloudHash = "", cloudDevice = ""
    var localSaved = Date(timeIntervalSince1970: 0), cloudSaved = Date(timeIntervalSince1970: 0)
    var localFiles = 0, cloudFiles = 0
    var cloudRead = false, conflict = false

    /// The two saves as the host compares them; nil with no cloud folder, no title ID, or a save that cannot be read. Blocks while an
    /// iCloud item that is only listed downloads (10 s at most): call it off the main thread.
    static func report() -> SaveSyncConflict? {
        // getenv, not ProcessInfo.environment: that is a snapshot taken before launcher.c reads env.txt and the saved choices
        guard let save = getenv("DM_SAVE") else { fputs("launcher: save sync: no DM_SAVE\n", stderr); return nil }
        let folder = getenv("DM_SAVE_SYNC").map { String(cString: $0) }
            ?? (UserDefaults.standard.string(forKey: "cloudSaves.folder") ?? "")
        guard !folder.isEmpty, !folder.hasPrefix("icloud") else { fputs("launcher: save sync: no cloud folder\n", stderr); return nil }
        guard let id = info("DMTitleID"), !id.isEmpty else { fputs("launcher: save sync: no title ID\n", stderr); return nil }
        let cloud = folder + "/" + id + "/save.dmsync"
        var buf = [CChar](repeating: 0, count: 1024)
        guard save_sync_report(save, cloud, id, &buf, buf.count) == 0 else {
            fputs("launcher: the save sync report could not be read\n", stderr)
            return nil
        }
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

    var localLabel: String { localFiles == 0 ? "This Mac has no save — use the cloud's" : "This Mac — saved \(Self.when(localSaved))" }
    var cloudLabel: String {
        guard cloudRead, cloudFiles > 0 else { return "iCloud has no save — use this Mac's" }
        return cloudDevice.isEmpty ? "iCloud — saved \(Self.when(cloudSaved))" : "iCloud from \(cloudDevice) — saved \(Self.when(cloudSaved))"
    }
    var message: String {
        "Played on both without syncing in between, so neither replaces the other until you pick one. The one you do not pick is kept in the save's backups folder."
    }
}

struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    let done: (Bool) -> Void
    let fit: (CGFloat) -> Void
    @AppStorage("window.show") private var show = true
    @AppStorage("window.advanced") private var advanced = false
    @State private var checking = false

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 64, height: 64).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(info("CFBundleName") ?? "Game").font(.title2.weight(.semibold))
                    Text("Version \(info("CFBundleShortVersionString") ?? "1.0")").foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding([.horizontal, .top], 20)
            Form {
                settingSections(basic: true)
                CloudSavesSection()
                Section {
                    DisclosureGroup("Advanced settings", isExpanded: $advanced) {}
                }
                if advanced {
                    settingSections(basic: false)
                    Section {
                        Button("Reset to tested defaults") { m.reset() }
                            .disabled(!m.changed)
                    } footer: {
                        Text("Brings back all settings this game was tested with.")
                    }
                }
            }
            .formStyle(.grouped)
            .onScrollGeometryChange(for: CGFloat.self) {
                $0.contentSize.height + $0.contentInsets.top + $0.contentInsets.bottom - $0.containerSize.height
            } action: { _, extraHeight in
                fit(extraHeight)
            }
            HStack {
                Toggle("Show this window at launch", isOn: $show).toggleStyle(.checkbox)
                Spacer()
                Button("Quit") { done(false) }
                Button("Play") { start() }.keyboardShortcut(.defaultAction)
            }
            .padding(20)
        }
        .frame(minWidth: 520, idealWidth: 560)
    }

    /// Play, after the cloud's save is compared with this Mac's (dm/host/save_sync.h save_sync_report): a save that changed on both sides is the
    /// player's call, not the newer file's. Anything else passes `ask` and leaves the host's own decision as it was.
    private func start() {
        guard !checking else { return }
        checking = true
        DispatchQueue.global(qos: .userInitiated).async {
            let c = SaveSyncConflict.report()
            DispatchQueue.main.async {
                checking = false
                // A report that could not be read leaves DM_SAVE_SYNC_CHOICE alone: `ask` with nothing compared would keep the cloud's
                // save out of this session, which is the one behaviour the player must never get by accident.
                guard let c else { fputs("launcher: no report; playing as before\n", stderr); done(true); return }
                if c.conflict {
                    fputs("launcher: both saves changed; asking\n", stderr)
                    let a = NSAlert()
                    a.messageText = "Both saves changed"
                    a.informativeText = c.message
                    a.addButton(withTitle: c.localLabel)     // first button = the default (Return)
                    a.addButton(withTitle: c.cloudLabel)
                    a.addButton(withTitle: "Cancel")
                    a.alertStyle = .warning
                    switch a.runModal() {
                    case .alertFirstButtonReturn: choose(local: true, c)
                    case .alertSecondButtonReturn: choose(local: false, c)
                    default: break                            // Cancel: back to the window, no choice made
                    }
                    return
                }
                setenv("DM_SAVE_SYNC_CHOICE", "ask", 1)
                done(true)
            }
        }
    }

    private func choose(local useLocal: Bool, _ c: SaveSyncConflict) {
        let choice = useLocal ? "local:" + c.localHash : "cloud:" + c.cloudHash
        setenv("DM_SAVE_SYNC_CHOICE", choice, 1)
        fputs("launcher: the player chose \(choice)\n", stderr)
        done(true)
    }

    @ViewBuilder private func settingSections(basic: Bool) -> some View {
        ForEach(m.sections(basic: basic), id: \.self) { section in
            let rows = m.rows(in: section, basic: basic)
            if !rows.isEmpty {
                Section(section) {
                    ForEach(rows) { SettingRow(s: $0, m: m) }
                }
            }
        }
    }
}

/// Cloud saves: a folder the save is synced through (dm/host/save_sync.h), the one the iPhone app picks in its launcher's Cloud saves section. It is kept in this app's
/// preferences as cloudSaves.folder, which launcher.c puts into DM_SAVE_SYNC before the host starts; a DM_SAVE_SYNC an env file or the launch environment set wins, and is shown here.
struct CloudSavesSection: View {
    @AppStorage("cloudSaves.folder") private var folder = ""
    private let fixed: String? = {
        let env = ProcessInfo.processInfo.environment
        let keys = ((env["DM_ENV_FILE_KEYS"] ?? "") + " " + (env["DM_ENV_LAUNCH_KEYS"] ?? "")).split(separator: " ")
        return keys.contains("DM_SAVE_SYNC") || env["DM_SAVE_SYNC"] != nil ? env["DM_SAVE_SYNC"] : nil
    }()

    var body: some View {
        Section {
            LabeledContent("Cloud saves") {
                Text(fixed.map { "\(Self.shown($0)) (env.txt)" } ?? (folder.isEmpty ? "Off" : Self.shown(folder)))
                    .foregroundStyle(.secondary).lineLimit(1).truncationMode(.head)
            }
            if fixed == nil {
                HStack {
                    Button(folder.isEmpty ? "Choose a folder…" : "Choose a different folder…", action: choose)
                    if !folder.isEmpty { Button("Turn off") { folder = "" } }
                }
            }
        } footer: {
            Text("Choose the iCloud Drive folder the iPhone app uses for this game. The save comes in when the game starts and goes out after each save; a save that is replaced is kept in the save's -backups folder.")
        }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.allowsMultipleSelection = false; panel.canCreateDirectories = true
        panel.prompt = "Use for Cloud Saves"
        panel.message = "The folder the iPhone app chose in its launcher's Cloud saves"
        panel.directoryURL = URL(fileURLWithPath: folder.isEmpty ? Self.drive : folder)
        if panel.runModal() == .OK, let url = panel.url { folder = url.path }
    }

    static let drive = NSHomeDirectory() + "/Library/Mobile Documents/com~apple~CloudDocs"   // iCloud Drive, as the Finder shows it

    /// A path under iCloud Drive as the Finder and the Files app name it: iCloud Drive › Documents › dev › Zelda; any other path as it is
    static func shown(_ path: String) -> String {
        guard path == drive || path.hasPrefix(drive + "/") else { return path }
        return (["iCloud Drive"] + path.dropFirst(drive.count).split(separator: "/").map(String.init)).joined(separator: " › ")
    }
}

private final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let model: LauncherModel
    var window: NSWindow?
    var play = false

    init(_ model: LauncherModel) { self.model = model }

    func applicationDidFinishLaunching(_ n: Notification) {
        let name = info("CFBundleName") ?? "Game"
        let menu = NSMenu(), item = NSMenuItem(), app = NSMenu()
        app.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        app.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = app; menu.addItem(item); NSApp.mainMenu = menu
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 660), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        w.title = name; w.isReleasedWhenClosed = false; w.delegate = self
        w.contentMinSize = NSSize(width: 520, height: 0)
        window = w
        if let screen = w.screen ?? NSScreen.main {
            w.setFrameTopLeftPoint(NSPoint(x: screen.visibleFrame.midX - w.frame.width / 2, y: screen.visibleFrame.maxY))
        }
        let host = NSHostingView(rootView: LauncherView(m: model, done: { [weak self] in self?.finish($0) }, fit: { [weak self] in self?.fit($0) }))
        host.sizingOptions = []   // the Form's measured scroll content owns the height, including after collapse
        w.contentView = host
        host.layoutSubtreeIfNeeded()
        // no activation and no ordering in front of another app's windows: LaunchServices activates an app opened from Finder or the Dock (applicationDidBecomeActive), open -g does not
        if NSApp.isActive { w.makeKeyAndOrderFront(nil) } else { w.orderBack(nil) }
    }
    func applicationDidBecomeActive(_ n: Notification) { window?.makeKeyAndOrderFront(nil) }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {   // ⌘Q, or a logout or restart: the app ends without the game (exit 0, as Quit)
        fputs("launcher: quit without starting the game\n", stderr)
        return .terminateNow   // not terminateCancel, which a logout or restart takes as a refusal and stops
    }
    func windowWillClose(_ n: Notification) { finish(false) }

    func fit(_ extraHeight: CGFloat) {
        guard let w = window, let screen = w.screen else { return }
        let frame = LauncherWindowLayout.fitted(w.frame, extraHeight: extraHeight, visible: screen.visibleFrame)
        if abs(frame.height - w.frame.height) > 0.5 { w.setFrame(frame, display: true) }
    }

    func finish(_ play: Bool) {
        guard let w = window else { return }
        window = nil; self.play = play
        // The main thread blocks in launcher.c's waitpid once run() returns, so the window must leave the screen now: no fade-out (it runs on later run-loop passes),
        // and dmMacLauncher flushes the order-out (AppKit commits it at the end of a pass). Without both, the window stayed on screen, not responding, until the game ended
        w.animationBehavior = .none
        w.delegate = nil; w.orderOut(nil)
        NSApp.stop(nil)
        NSApp.postEvent(NSEvent.otherEvent(with: .applicationDefined, location: .zero, modifierFlags: [], timestamp: 0, windowNumber: 0, context: nil, subtype: 0, data1: 0, data2: 0)!, atStart: true)   // run returns at the next event
    }
}

enum LauncherWindowLayout {
    static func fitted(_ frame: NSRect, extraHeight: CGFloat, visible: NSRect) -> NSRect {
        let top = min(frame.maxY, visible.maxY)
        let height = min(frame.height + extraHeight, top - visible.minY)
        return NSRect(x: frame.minX, y: top - height, width: frame.width, height: height)
    }
}

// 1: Play (the window's settings are in the environment), 0: the player quit, 2: no window (no launcher.json, or "Show this window at launch" off): launcher.c applies the saved choices
@_cdecl("dm_mac_launcher")
public func dmMacLauncher() -> Int32 {
    guard let url = Bundle.main.resourceURL?.appendingPathComponent("dm/launcher.json"), let data = try? Data(contentsOf: url) else { return 2 }
    guard let file = try? JSONDecoder().decode(SettingsFile.self, from: data), !file.settings.isEmpty else { fputs("[RED] launcher: dm/launcher.json cannot be read; starting the game\n", stderr); return 2 }
    if (UserDefaults.standard.object(forKey: "window.show") as? Bool) == false && !NSEvent.modifierFlags.contains(.option) {
        fputs("launcher: settings window off (hold Option at launch to show it)\n", stderr)
        return 2
    }
    let model = LauncherModel(file.settings)
    let app = NSApplication.shared, launcher = Launcher(model)
    app.setActivationPolicy(.regular)
    app.delegate = launcher
    fputs("launcher: \(file.settings.count) settings shown\n", stderr)
    app.run()
    CATransaction.flush()   // finish's order-out
    app.setActivationPolicy(.prohibited)   // the host is the game's app from here on
    guard launcher.play else { fputs("launcher: quit without starting the game\n", stderr); return 0 }
    model.apply()
    return 1
}
