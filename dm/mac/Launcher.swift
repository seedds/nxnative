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

struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    let done: (Bool) -> Void
    let fit: (CGFloat) -> Void
    @AppStorage("window.show") private var show = true
    @AppStorage("window.advanced") private var advanced = false

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 44, height: 44).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 1) {
                    Text(info("CFBundleName") ?? "Game").font(.headline)
                    HStack(spacing: 0) {
                        Text("Version \(info("CFBundleShortVersionString") ?? "1.0")")
                        if let c = info("DMCommit"), !c.isEmpty {
                            Text(" (")
                            CommitLink(commit: c, url: info("DMCommitURL").flatMap(URL.init(string:)))
                            Text(")")
                        }
                    }
                    .font(.callout).foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding(.horizontal, 20).padding(.top, 2)   // the title bar's safe area already keeps it below the traffic lights
            Form { settings }
            .formStyle(.grouped)
            .contentMargins(.top, 6, for: .scrollContent)
            .background(OverlayScrollers())
            .onScrollGeometryChange(for: CGFloat.self) {
                $0.contentSize.height + $0.contentInsets.top + $0.contentInsets.bottom - $0.containerSize.height
            } action: { _, extraHeight in
                fit(extraHeight)
            }
            HStack {
                Toggle("Show this window at launch", isOn: $show).toggleStyle(.checkbox)
                Spacer()
                Button("Quit") { done(false) }
                Button("Play") { done(true) }.keyboardShortcut(.defaultAction)
            }
            .padding(20)
        }
        .frame(minWidth: 520, idealWidth: 560)
    }

    // Basic settings and the Advanced settings row share one card; the advanced ones open in a second card below it
    @ViewBuilder private var settings: some View {
        Section {
            settingRows(basic: true)
            Button { advanced.toggle() } label: {
                HStack {
                    Text("Advanced settings")
                    Spacer()
                    Image(systemName: advanced ? "chevron.down" : "chevron.right").foregroundStyle(.secondary).accessibilityHidden(true)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityValue(advanced ? "Shown" : "Hidden")
        }
        if advanced {
            Section { settingRows(basic: false) }
            Section {
                Button("Reset to tested defaults") { m.reset() }
                    .disabled(!m.changed)
            } footer: {
                Text("Brings back all settings this game was tested with.")
            }
        }
        CloudSavesSection()
    }

    /// The rows of each of the list's sections, under the section's name as a small caption inside the card
    @ViewBuilder private func settingRows(basic: Bool) -> some View {
        ForEach(m.sections(basic: basic), id: \.self) { section in
            let rows = m.rows(in: section, basic: basic)
            if !rows.isEmpty {
                if !section.isEmpty {
                    Text(section.uppercased()).font(.caption.weight(.semibold)).foregroundStyle(.secondary).padding(.top, 4).accessibilityAddTraits(.isHeader)
                }
                ForEach(rows) { SettingRow(s: $0, m: m) }
            }
        }
    }
}

/// "Version 1.6.0 (d0ab754c)": the commit that packed the app (DMCommit, pack_app.py source_commit) beside the game's version. A link to the commit's page when the packer
/// found it on GitHub (DMCommitURL; a local commit has no page there): blue under the pointer, a click opens it. Its menu copies the commit ID and the link
struct CommitLink: View {
    let commit: String, url: URL?
    @State private var hover = false

    var body: some View {
        let id = commit.hasSuffix("+dirty") ? String(commit.dropLast(6)) : commit
        Button { if let url { NSWorkspace.shared.open(url) } } label: {
            Text("\(id.prefix(8))\(commit.hasSuffix("+dirty") ? "+dirty" : "")")
                .foregroundStyle(url != nil && hover ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.secondary))
        }
        .buttonStyle(.plain).focusable(false)
        .onHover { inside in
            hover = inside
            guard url != nil else { return }
            if inside { NSCursor.pointingHand.push() } else { NSCursor.pop() }
        }
        .help(url != nil ? "Open this commit on GitHub" : "This commit is not on GitHub")
        .contextMenu {
            Button("Copy Commit ID") { Self.copy(id) }
            if let url { Button("Copy Link") { Self.copy(url.absoluteString) } }
        }
    }

    private static func copy(_ s: String) { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(s, forType: .string) }
}


/// The Form's scroll bar as an overlay: thin, shown while scrolling or under the pointer, whatever "Show scroll bars" says in System Settings
/// (with a mouse connected, the default there is a permanent wide bar). AppKit sets the style again when that setting changes, so it is set again then
struct OverlayScrollers: NSViewRepresentable {
    final class Probe: NSView {
        private var token: NSObjectProtocol?
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            apply()
            if token == nil {
                token = NotificationCenter.default.addObserver(forName: NSScroller.preferredScrollerStyleDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
                    DispatchQueue.main.async { self?.apply() }
                }
            }
        }
        deinit { if let t = token { NotificationCenter.default.removeObserver(t) } }
        func apply() {
            DispatchQueue.main.async { [weak self] in
                guard let root = self?.window?.contentView else { return }
                func walk(_ v: NSView) {
                    if let s = v as? NSScrollView {
                        s.scrollerStyle = .overlay; s.autohidesScrollers = true
                        s.verticalScroller?.controlSize = .small
                    }
                    v.subviews.forEach(walk)
                }
                walk(root)
            }
        }
    }
    func makeNSView(context: Context) -> Probe { Probe() }
    func updateNSView(_ v: Probe, context: Context) { v.apply() }
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
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 660), styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        w.title = name; w.isReleasedWhenClosed = false; w.delegate = self
        w.titlebarAppearsTransparent = true; w.titleVisibility = .hidden; w.titlebarSeparatorStyle = .none   // one surface with the header, which shows the name
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
    // No control holds the focus when the window first becomes key, so none shows a focus ring until the player presses Tab. AppKit gives the first key view the focus
    // then (with keyboard navigation on in System Settings, a ring around a button); Return still presses Play, the window's default button
    private var focusCleared = false
    func windowDidBecomeKey(_ n: Notification) {
        guard !focusCleared, let w = window else { return }
        focusCleared = true
        DispatchQueue.main.async { w.makeFirstResponder(nil) }
    }
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
