// Mac app launcher: the settings window shown before the game starts, in the launcher's own process (launcher.c calls dm_mac_launcher before it starts the host). Its settings come from the
// app's Resources/dm/launcher.json (dm/games/settings.py with SETTINGS_PLATFORM=mac); the model and the rows are the iOS launcher's (dm/ios/Sources/LauncherSettings.swift, LauncherRow.swift),
// and so is the store: launcher.<VAR> in the app's preferences, which LauncherModel reads on both windowed and no-window launches (docs/frontends.md, Saved choices). Basic settings (level basic) come
// first, the others under Advanced settings, each in the list's sections. Play puts the settings into this process's environment, which the host inherits; Quit and closing the window end the
// app without the game. "Show this window at launch" off: no window unless Option is held at launch, and LauncherModel applies the saved choices and launcher.json defaults without saving. The window never takes focus by itself: an app
// opened in the background (open -g) keeps its window behind until the player switches to it
import AppKit
import AVFAudio
import CoreAudio
import QuartzCore
import SwiftUI

private func info(_ key: String) -> String? { Bundle.main.object(forInfoDictionaryKey: key) as? String }

/// The host's comparison of the two saves (dm/host/save_sync.c). This launcher links save_sync.o but imports no C header (pack_app.py builds it with swiftc and no bridging
/// header), so the symbol is declared here.
@_silgen_name("save_sync_report")
private func save_sync_report(_ save: UnsafePointer<CChar>, _ title: UnsafePointer<CChar>, _ folder: UnsafePointer<CChar>?, _ waitMs: Int32,
                              _ out: UnsafeMutablePointer<CChar>, _ n: Int) -> Int32

/// Cloud saves before Play: this Mac's save against the cloud's (SaveSyncReport, shared with the iPhone launcher), and the question it needs, as alerts.
/// It runs with the window's Play and, with the window off, before dmMacLauncher returns; DM_LAUNCHER=0 asks nothing (the host takes the newer save).
enum SaveCheck {
    enum Outcome { case play(String?), retry, cancel }

    /// The folder as launcher.c will pass it (cloud_saves_folder): DM_SAVE_SYNC from the launch environment or an env file, else the window's choice; "" or "0" = off.
    static func effectiveFolder(defaults: UserDefaults = .standard) -> String {
        getenv("DM_SAVE_SYNC").map { String(cString: $0) } ?? (defaults.string(forKey: "cloudSaves.folder") ?? "")
    }

    /// Blocks while the cloud's file comes (SaveSyncReport.waitMs at most): call it off the main thread. nil: no report (sync is then left to the host as before)
    static func report(folder: String? = nil) -> SaveSyncReport? {
        // getenv, not ProcessInfo.environment: launcher.c sets DM_SAVE and applies the env files in this process before the window
        guard let save = getenv("DM_SAVE") else { fputs("launcher: save sync: no DM_SAVE\n", stderr); return nil }
        guard let id = info("DMTitleID"), !id.isEmpty else { fputs("launcher: save sync: no DMTitleID\n", stderr); return nil }
        let folder = folder ?? effectiveFolder()
        var buf = [CChar](repeating: 0, count: 1024)
        let r = (folder.isEmpty ? "0" : folder).withCString { save_sync_report(save, id, $0, SaveSyncReport.waitMs, &buf, buf.count) }
        let line = String(cString: buf)
        guard r == 0 else { fputs("launcher: save sync: no report on \(String(cString: save))\n", stderr); return nil }
        fputs("launcher: save sync: \(line)\n", stderr)
        return SaveSyncReport(line: line)
    }

    /// The question the report needs, as a modal alert; `cancel` names the way back ("Cancel" to the window, "Quit" without one)
    static func ask(_ r: SaveSyncReport?, cancel: String) -> Outcome {
        switch SaveSyncReport.step(r) {
        case .play(let choice): return .play(choice)
        case .choose:
            guard let r else { return .play(nil) }
            let a = NSAlert()
            a.messageText = SaveSyncReport.chooseTitle
            a.informativeText = r.chooseMessage(here: "Mac")
            a.alertStyle = .warning
            a.addButton(withTitle: r.localLabel(here: "Mac"))
            a.addButton(withTitle: r.cloudLabel)
            a.addButton(withTitle: cancel)
            a.buttons[0].keyEquivalent = ""   // no default: Return must not pick a save (the newer is not always the one to keep)
            a.buttons[2].keyEquivalent = "\u{1b}"
            switch a.runModal() {
            case .alertFirstButtonReturn: return .play(r.localChoice)
            case .alertSecondButtonReturn: return .play(r.cloudChoice)
            default: return .cancel
            }
        case .missing:
            guard let r else { return .play(nil) }
            let a = NSAlert()
            a.messageText = SaveSyncReport.missingTitle
            a.informativeText = r.missingMessage(here: "Mac")
            a.alertStyle = .warning
            a.addButton(withTitle: "Try Again")
            a.addButton(withTitle: "Play Without Sending")
            a.addButton(withTitle: r.sendLabel(here: "Mac"))
            a.addButton(withTitle: cancel)
            a.buttons[2].hasDestructiveAction = true
            a.buttons[3].keyEquivalent = "\u{1b}"
            switch a.runModal() {
            case .alertFirstButtonReturn: return .retry
            case .alertSecondButtonReturn: return .play("offline")   // this Mac's save, not sent: the next start compares again
            case .alertThirdButtonReturn: return .play(r.localChoice)   // local:none
            default: return .cancel
            }
        case .unreadable:
            let a = NSAlert()
            a.messageText = SaveSyncReport.unreadableTitle
            a.informativeText = r?.unreadableMessage(here: "Mac") ?? ""
            a.addButton(withTitle: "Play This Mac's Save")
            a.addButton(withTitle: "Try Again")
            a.addButton(withTitle: cancel)
            a.buttons[2].keyEquivalent = "\u{1b}"
            switch a.runModal() {
            case .alertFirstButtonReturn: return .play("offline")   // the mount does not wait for the cloud again
            case .alertSecondButtonReturn: return .retry
            default: return .cancel
            }
        }
    }

    static func apply(_ choice: String?) {
        guard let choice else { return }
        setenv("DM_SAVE_SYNC_CHOICE", choice, 1)
        fputs("launcher: DM_SAVE_SYNC_CHOICE=\(choice)\n", stderr)
    }

    /// With the window off: the check and its question before the game, the app in front only while it asks. false: the player quit
    static func withoutWindow(defaults: UserDefaults = .standard) -> Bool {
        let folder = effectiveFolder(defaults: defaults)
        guard SaveSyncReport.syncEnabled(folder) else { return true }
        final class Box: @unchecked Sendable { let lock = NSLock(); var report: SaveSyncReport?; var waiting = false }
        let app = NSApplication.shared
        defer { app.setActivationPolicy(.prohibited) }
        while true {
            let box = Box(), sem = DispatchSemaphore(value: 0)
            DispatchQueue.global(qos: .userInitiated).async {
                let r = report(folder: folder)
                box.lock.lock(); box.report = r; box.lock.unlock()
                sem.signal()
                DispatchQueue.main.async { if box.waiting { app.abortModal() } }   // runs inside the waiting alert's modal loop, if there is one
            }
            if sem.wait(timeout: .now() + 0.5) == .timedOut {   // a cloud file to download: say so, and let the player go on without it
                app.setActivationPolicy(.regular); app.activate(ignoringOtherApps: true)
                let a = NSAlert()
                a.messageText = "Checking the cloud save…"
                a.informativeText = "Waiting for iCloud to deliver this game's save."
                a.addButton(withTitle: "Play Without It")
                a.addButton(withTitle: "Quit")
                box.waiting = true
                let answer = a.runModal()
                box.waiting = false
                if answer == .alertFirstButtonReturn { apply("offline"); return true }
                if answer == .alertSecondButtonReturn { return false }
                sem.wait()   // aborted: the report is in
            }
            box.lock.lock(); let r = box.report; box.lock.unlock()
            if case .play(let choice) = SaveSyncReport.step(r) { apply(choice); return true }
            app.setActivationPolicy(.regular); app.activate(ignoringOtherApps: true)
            switch ask(r, cancel: "Quit") {
            case .play(let choice): apply(choice); return true
            case .retry: continue
            case .cancel: return false
            }
        }
    }
}

struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    let done: (Bool) -> Void
    let fit: (CGFloat) -> Void
    @AppStorage("window.show") private var show = true
    @AppStorage("window.advanced") private var advanced = false
    @State private var checking = false
    @State private var check = 0   // which Play the running check is for: one the player went on from without it is dropped

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
                if m.values["DM_MIC"] == "1" { MicrophoneDevicePicker() }
                CloudSavesSection()
                PrewarmUploadSetting()
                if !m.sections(basic: true).contains("Controls") { KeyboardSection() }   // below Controls (settingSections), else here
                Section {
                    // a Button, not an empty DisclosureGroup: the sections below live outside it, and in a grouped Form a DisclosureGroup does not toggle on row/label clicks (and failed to open on macOS 27)
                    Button { advanced.toggle() } label: {
                        HStack {
                            Image(systemName: "chevron.right").rotationEffect(.degrees(advanced ? 90 : 0)).foregroundStyle(.secondary).accessibilityHidden(true)
                            Text("Advanced settings")
                            Spacer()
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityValue(advanced ? "Expanded" : "Collapsed")
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
                if checking {
                    ProgressView().controlSize(.small)
                    Text("Checking the cloud save…").foregroundStyle(.secondary)
                    Button("Play Without It") { checking = false; SaveCheck.apply("offline"); done(true) }
                }
                Button("Quit") { done(false) }
                Button("Play") { start() }.keyboardShortcut(.defaultAction).disabled(checking)
            }
            .padding(20)
        }
        .frame(minWidth: 520, idealWidth: 560)
    }

    /// Play, once the cloud's save is compared with this Mac's: a save that changed on both sides is the player's pick, not the newer file's (SaveCheck)
    private func start() {
        let folder = SaveCheck.effectiveFolder()
        guard SaveSyncReport.syncEnabled(folder) else { done(true); return }
        check += 1; checking = true
        let mine = check
        DispatchQueue.global(qos: .userInitiated).async {
            let r = SaveCheck.report(folder: folder)
            DispatchQueue.main.async {
                guard checking, check == mine else { return }   // the player played without it
                checking = false
                switch SaveCheck.ask(r, cancel: "Cancel") {
                case .play(let choice): SaveCheck.apply(choice); done(true)
                case .retry: start()
                case .cancel: break   // back to the window, nothing chosen
                }
            }
        }
    }

    @ViewBuilder private func settingSections(basic: Bool) -> some View {
        ForEach(m.sections(basic: basic), id: \.self) { section in
            let rows = m.rows(in: section, basic: basic)
            if !rows.isEmpty {
                Section(section) {
                    ForEach(rows) { SettingRow(s: $0, m: m) }
                }
            }
            if basic && section == "Controls" { KeyboardSection() }   // the keyboard map, right below the controls
        }
    }
}

private struct MicrophoneDevicePicker: View {
    @AppStorage("launcher.DM_MIC_DEVICE") private var savedDevice = "default"
    @State private var devices = MicrophoneInputDevices.list()

    var body: some View {
        Section {
            if let uid = getenv("DM_MIC_DEVICE") {
                let value = String(cString: uid)
                LabeledContent("Microphone input", value: devices.first { $0.uid == value }?.name ?? "System Default")
            } else {
                Picker("Microphone input", selection: $savedDevice) {
                    Text("System Default").tag("default")
                    if savedDevice != "default", !devices.contains(where: { $0.uid == savedDevice }) {
                        Text("Selected device unavailable").tag(savedDevice)
                    }
                    ForEach(devices) { Text($0.name).tag($0.uid) }
                }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            devices = MicrophoneInputDevices.list()
        }
    }
}

private struct MicrophoneInputDevice: Identifiable, Hashable {
    let uid: String
    let name: String
    var id: String { uid }
}

private enum MicrophoneInputDevices {
    static func list() -> [MicrophoneInputDevice] {
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr,
              size >= UInt32(MemoryLayout<AudioDeviceID>.size) else { return [] }
        var ids = [AudioDeviceID](repeating: kAudioObjectUnknown, count: Int(size) / MemoryLayout<AudioDeviceID>.stride)
        let status = ids.withUnsafeMutableBytes { bytes in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, bytes.baseAddress!)
        }
        guard status == noErr else { return [] }
        return ids.compactMap { device in
            guard hasInput(device), let uid = string(device, selector: kAudioDevicePropertyDeviceUID),
                  let name = string(device, selector: kAudioObjectPropertyName), !uid.isEmpty else { return nil }
            return MicrophoneInputDevice(uid: uid, name: dualSenseOverUSB(device) ? "\(name) (USB)" : name)
        }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    private static func hasInput(_ device: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration,
                                                 mScope: kAudioDevicePropertyScopeInput,
                                                 mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(device, &address, 0, nil, &size) == noErr,
              size >= UInt32(MemoryLayout<AudioBufferList>.size) else { return false }
        let data = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { data.deallocate() }
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, data) == noErr else { return false }
        let buffers = UnsafeMutableAudioBufferListPointer(data.assumingMemoryBound(to: AudioBufferList.self))
        return buffers.contains { $0.mNumberChannels > 0 }
    }

    // A DualSense or DualSense Edge on USB: CoreAudio exposes its USB VID:PID at the end of ModelUID ("<name>:054C:0CE6" / ":0DF2").
    private static func dualSenseOverUSB(_ device: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyTransportType,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var transport: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &transport) == noErr,
              transport == kAudioDeviceTransportTypeUSB,
              let model = string(device, selector: kAudioDevicePropertyModelUID) else { return false }
        let parts = model.split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count >= 3,
              let vid = UInt32(parts[parts.count - 2], radix: 16),
              let pid = UInt32(parts[parts.count - 1], radix: 16) else { return false }
        return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2)
    }

    private static func string(_ device: AudioDeviceID, selector: AudioObjectPropertySelector) -> String? {
        var address = AudioObjectPropertyAddress(mSelector: selector,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var value: CFString?
        var size = UInt32(MemoryLayout<CFString?>.size)
        let status = withUnsafeMutablePointer(to: &value) { pointer in
            AudioObjectGetPropertyData(device, &address, 0, nil, &size, UnsafeMutableRawPointer(pointer))
        }
        guard status == noErr, let value else { return nil }
        return value as String
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

// Called on the main thread after settings and cloud-save decisions. The OS can present
// its consent alert even when the settings window is suppressed.
@_cdecl("dm_mac_microphone_consent")
public func dmMacMicrophoneConsent() -> Int32 {
    switch AVAudioApplication.shared.recordPermission {
    case .granted: return 1
    case .undetermined:
        _ = NSApplication.shared
        var allowed: Bool?
        AVAudioApplication.requestRecordPermission { granted in
            DispatchQueue.main.async { allowed = granted }
        }
        while allowed == nil {
            RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.05))
        }
        if allowed == true {
            fputs("launcher: microphone permission allowed\n", stderr)
            return 1
        }
    default: break
    }
    fputs("launcher: microphone permission denied\n", stderr)
    return 0
}

private var cachedLauncherSettings: [Setting]?
private var loadedLauncherSettings = false

private func launcherSettings() -> [Setting]? {
    if loadedLauncherSettings { return cachedLauncherSettings }
    loadedLauncherSettings = true
    guard let url = Bundle.main.resourceURL?.appendingPathComponent("dm/launcher.json"),
          let data = try? Data(contentsOf: url) else { return nil }
    guard let file = try? JSONDecoder().decode(SettingsFile.self, from: data) else {
        fputs("[RED] launcher: dm/launcher.json cannot be read; starting the game\n", stderr)
        return nil
    }
    cachedLauncherSettings = file.settings
    return file.settings
}

// Record which settings in dm/launcher.json are already set in the launch environment, before launcher.c reads env.txt.
@_cdecl("dm_mac_record_launch_keys")
public func dmMacRecordLaunchKeys() {
    let settings = launcherSettings() ?? []
    let keys = settings.map(\.env).filter { getenv($0) != nil }
    setenv("DM_ENV_LAUNCH_KEYS", keys.joined(separator: " "), 1)
}

// 1: start the game (resolved settings from dm/launcher.json are in the environment), 0: the player quit.
// showWindow == 0 (DM_LAUNCHER=0 or --no-launcher): resolve and apply saved choices + launcher.json defaults without initializing AppKit or SaveCheck.
@_cdecl("dm_mac_launcher")
public func dmMacLauncher(_ showWindow: Int32) -> Int32 {
    guard let settings = launcherSettings(), !settings.isEmpty else { return 1 }
    if showWindow == 0 {
        LauncherModel(settings).apply(persist: false)
        return 1
    }
    if (UserDefaults.standard.object(forKey: "window.show") as? Bool) == false && !NSEvent.modifierFlags.contains(.option) {
        fputs("launcher: settings window off (hold Option at launch to show it)\n", stderr)
        guard SaveCheck.withoutWindow() else { return 0 }
        LauncherModel(settings).apply(persist: false)
        return 1
    }
    let model = LauncherModel(settings)
    let app = NSApplication.shared, launcher = Launcher(model)
    app.setActivationPolicy(.regular)
    app.delegate = launcher
    fputs("launcher: \(settings.count) settings shown\n", stderr)
    app.run()
    CATransaction.flush()   // finish's order-out
    app.setActivationPolicy(.prohibited)   // the host is the game's app from here on
    guard launcher.play else { fputs("launcher: quit without starting the game\n", stderr); return 0 }
    model.apply(persist: true)
    return 1
}

// The keyboard map in the Mac settings window: the Keyboard section (default or custom, and a button that opens the map) and the map's sheet, where each button and stick direction
// of the controller shows its key; click one and press a key to bind it, or hold Shift, Control or Option and press a key for a chord (Esc stops without a change). The map itself is the host's (dm/host/keymap.h, reached through launcher.c's
// dm_km_* functions: this launcher imports no C header). It is kept as the saved choice launcher.DM_KEYMAP in keymap.h's full form, absent while it is the default (docs/frontends.md, Saved choices);
// launcher.c checks it with the host's parser and makes it DM_KEYMAP before the host starts, with or without this window. A DM_KEYMAP the launch environment or an env file set wins, and is shown here as fixed.

@_silgen_name("dm_km_target_label") private func dm_km_target_label(_ t: Int32) -> UnsafePointer<CChar>
@_silgen_name("dm_km_slot_label") private func dm_km_slot_label(_ slot: Int32) -> UnsafePointer<CChar>
@_silgen_name("dm_km_slots") private func dm_km_slots() -> Int32
@_silgen_name("dm_km_format_max") private func dm_km_format_max() -> Int32
@_silgen_name("dm_km_bind") private func dm_km_bind(_ key: UnsafeMutablePointer<UInt8>, _ target: Int32, _ slot: Int32, _ add: Int32) -> Int32
@_silgen_name("dm_km_slot") private func dm_km_slot(_ code: Int32, _ mods: Int32) -> Int32
@_silgen_name("dm_km_groups") private func dm_km_groups() -> Int32
@_silgen_name("dm_km_group_title") private func dm_km_group_title(_ g: Int32) -> UnsafePointer<CChar>
@_silgen_name("dm_km_group_size") private func dm_km_group_size(_ g: Int32) -> Int32
@_silgen_name("dm_km_group_target") private func dm_km_group_target(_ g: Int32, _ i: Int32) -> Int32
@_silgen_name("dm_km_keys") private func dm_km_keys() -> Int32
@_silgen_name("dm_km_bindable") private func dm_km_bindable(_ code: Int32) -> Int32
@_silgen_name("dm_km_read") private func dm_km_read(_ map: UnsafePointer<CChar>?, _ key: UnsafeMutablePointer<UInt8>, _ src: UnsafeMutablePointer<Int32>, _ speed: UnsafeMutablePointer<Float>, _ invert: UnsafeMutablePointer<Int32>) -> Int32
@_silgen_name("dm_km_outs") private func dm_km_outs() -> Int32
@_silgen_name("dm_km_out_label") private func dm_km_out_label(_ o: Int32) -> UnsafePointer<CChar>
@_silgen_name("dm_km_src_off") private func dm_km_src_off() -> Int32
@_silgen_name("dm_km_src_mouse") private func dm_km_src_mouse() -> Int32
@_silgen_name("dm_km_mouse_key") private func dm_km_mouse_key(_ button: Int32) -> Int32
@_silgen_name("dm_km_src_label") private func dm_km_src_label(_ src: Int32) -> UnsafePointer<CChar>
@_silgen_name("dm_km_mouse_set") private func dm_km_mouse_set(_ src: UnsafeMutablePointer<Int32>, _ out: Int32, _ v: Int32)
@_silgen_name("dm_km_presets_load") private func dm_km_presets_load() -> Int32
@_silgen_name("dm_km_preset_save") private func dm_km_preset_save(_ key: UnsafePointer<UInt8>, _ src: UnsafePointer<Int32>, _ speed: UnsafePointer<Float>, _ invert: UnsafePointer<Int32>, _ name: UnsafePointer<CChar>, _ path: UnsafeMutablePointer<CChar>, _ n: Int) -> Int32
@_silgen_name("dm_km_preset_dir") private func dm_km_preset_dir() -> UnsafePointer<CChar>
@_silgen_name("dm_km_preset_name") private func dm_km_preset_name(_ i: Int32) -> UnsafePointer<CChar>
@_silgen_name("dm_km_preset_map") private func dm_km_preset_map(_ i: Int32) -> UnsafePointer<CChar>
@_silgen_name("dm_km_write") private func dm_km_write(_ key: UnsafePointer<UInt8>, _ src: UnsafePointer<Int32>, _ speed: UnsafePointer<Float>, _ invert: UnsafePointer<Int32>, _ out: UnsafeMutablePointer<CChar>, _ n: Int) -> Int32

/// A keyboard map: the target each binding presses (keymap.h: a binding is layer * keys + key code, the layers plain, Shift, Control, Option; 0xff none; mouse buttons from
/// key code 128), and what mouse movement moves: each output's source, speed and Invert Y (keymap.h MOUSERSTICK, MOUSELSTICK, MOUSEMOTION)
struct KeyMap: Equatable {
    static let none: UInt8 = 0xff
    static let slots = Int(dm_km_slots()), keyCount = Int(dm_km_keys()), outs = Int(dm_km_outs())
    static let srcOff = dm_km_src_off(), srcMouse = dm_km_src_mouse()   // an output's source: none, the plain mouse; else the key to hold (keymap.h KM_SRC_*)
    static let outputs = (0..<dm_km_outs()).map { String(cString: dm_km_out_label($0)) }   // keymap.h KM_OUT_*
    static func mouseKey(_ button: Int) -> Int? { let k = dm_km_mouse_key(Int32(button)); return k < 0 ? nil : Int(k) }   // a mouse button's key code
    var key = [UInt8](repeating: KeyMap.none, count: KeyMap.slots)
    var mouseSrc = [Int32](repeating: KeyMap.srcOff, count: KeyMap.outs)
    var mouseSpeed = [Float](repeating: 0, count: KeyMap.outs)   // all three filled by init (keymap.h km_default)
    var mouseInvert = [Int32](repeating: 0, count: KeyMap.outs)
    static func srcSlot(_ o: Int) -> Int { -10 - o }   // the capture slot of an output's mouse key (row targets are 0...)

    /// keymap.h's DM_KEYMAP over the default map (an empty string: the default)
    init(_ map: String = "") {
        _ = map.withCString { s in key.withUnsafeMutableBufferPointer { k in dm_km_read(s, k.baseAddress!, &mouseSrc, &mouseSpeed, &mouseInvert) } }
    }
    /// An output's source; another output with the plain mouse or the same key gets none (keymap.h km_mouse_set)
    mutating func setSource(_ o: Int, _ v: Int32) { dm_km_mouse_set(&mouseSrc, Int32(o), v) }
    static func label(source v: Int32) -> String { String(cString: dm_km_src_label(v)) }

    /// The full form ("A=j B=k ... SHAKE=space"), as launcher.DM_KEYMAP keeps it
    var string: String {
        var out = [CChar](repeating: 0, count: Int(dm_km_format_max()))   // any map's full form fits
        _ = key.withUnsafeBufferPointer { dm_km_write($0.baseAddress!, mouseSrc, mouseSpeed, mouseInvert, &out, out.count) }
        return String(cString: out)
    }
    func keys(_ target: Int) -> [Int] { (0..<KeyMap.slots).filter { key[$0] == UInt8(target) } }
    /// The binding (a key or a chord) presses target, which keeps no other unless add; returns the target that had the key before, if another (keymap.h km_bind)
    @discardableResult mutating func bind(_ target: Int, _ code: Int, add: Bool = false) -> Int? {
        let was = key.withUnsafeMutableBufferPointer { dm_km_bind($0.baseAddress!, Int32(target), Int32(code), add ? 1 : 0) }
        return was < 0 ? nil : Int(was)
    }
    mutating func clear(_ target: Int) { _ = key.withUnsafeMutableBufferPointer { dm_km_bind($0.baseAddress!, Int32(target), -1, 0) } }

    static let standard = KeyMap()
    /// The presets, the in-game panel's too: Default and the keymap files of the presets folder (keymap.h), read now: (name, map)
    static func presets() -> [(String, KeyMap)] {
        (0..<Int(dm_km_presets_load())).map { (String(cString: dm_km_preset_name(Int32($0))), KeyMap(String(cString: dm_km_preset_map(Int32($0))))) }
    }
    static var presetsFolder: URL { URL(fileURLWithPath: String(cString: dm_km_preset_dir())) }
    /// Saved as a keymap file named name (one of that name replaced); the file's path, or nil
    func savePreset(_ name: String) -> String? {
        var path = [CChar](repeating: 0, count: 1024)
        let r = key.withUnsafeBufferPointer { k in name.withCString { dm_km_preset_save(k.baseAddress!, mouseSrc, mouseSpeed, mouseInvert, $0, &path, path.count) } }
        return r == 0 ? String(cString: path) : nil
    }

    static func label(target: Int) -> String { String(cString: dm_km_target_label(Int32(target))) }
    static func label(key slot: Int) -> String { String(cString: dm_km_slot_label(Int32(slot))) }   // ⇧↑ for a chord
    static func bindable(_ code: Int) -> Bool { dm_km_bindable(Int32(code)) != 0 }

    /// The sheet's groups: keymap.h's (km_groups), the panel's and the keymap files' order
    static let groups: [(String, [Int])] = (0..<dm_km_groups()).map { g in
        (String(cString: dm_km_group_title(g)), (0..<dm_km_group_size(g)).map { Int(dm_km_group_target(g, $0)) })
    }
    /// A key pressed with the event's modifiers as a binding: the chord of a modifier held (keymap.h km_layer), else the key
    static func slot(_ code: Int, _ f: NSEvent.ModifierFlags) -> Int {
        Int(dm_km_slot(Int32(code), (f.contains(.shift) ? 1 : 0) | (f.contains(.control) ? 2 : 0) | (f.contains(.option) ? 4 : 0)))
    }
}

/// The section in the settings window: what the map is, and the button that opens it
struct KeyboardSection: View {
    @AppStorage("launcher.DM_KEYMAP") private var saved = ""
    @State private var editing = false
    @State private var presets: [(String, KeyMap)] = []
    private let fixed: String? = {
        let env = ProcessInfo.processInfo.environment
        let keys = ((env["DM_ENV_FILE_KEYS"] ?? "") + " " + (env["DM_ENV_LAUNCH_KEYS"] ?? "")).split(separator: " ")
        return keys.contains("DM_KEYMAP") || env["DM_KEYMAP"] != nil ? env["DM_KEYMAP"] : nil
    }()

    private var summary: String {
        if fixed != nil { return "Set by an env file" }
        let m = KeyMap(saved)
        return presets.first { $0.1 == m }?.0 ?? "Custom"
    }

    var body: some View {
        Section {
            LabeledContent("Keyboard and mouse") {
                HStack {
                    Text(summary).foregroundStyle(.secondary)
                    Button("Customize…") { editing = true }.disabled(fixed != nil)
                }
            }
        } footer: {
            Text("Choose which key or mouse button presses each button and stick direction, and what mouse movement moves. Keys are kept by their place on the keyboard.")
        }
        .sheet(isPresented: $editing, onDismiss: { presets = KeyMap.presets() }) { KeyMapSheet(saved: $saved) }
        .onAppear { presets = KeyMap.presets() }
    }
}

/// The map: one row per target, its key on a button; click it, then press a key
struct KeyMapSheet: View {
    @Binding var saved: String
    @Environment(\.dismiss) private var dismiss
    @State private var map = KeyMap()
    @State private var capturing: Int?
    @State private var adding = false   // the key awaited joins the target's (Add Key…)
    @State private var status = ""
    @State private var monitor: Any?
    @State private var presets: [(String, KeyMap)] = []
    @State private var size = KeyMapSheet.fit()

    /// The sheet's size from the settings window's, when it opens (a sheet keeps one size): three columns of cards in a wide window, two or one in a narrow
    private static func fit() -> CGSize {
        let w = NSApp.mainWindow?.contentLayoutRect.size ?? CGSize(width: 760, height: 800)
        return CGSize(width: min(1060, max(380, w.width - 40)), height: min(720, max(420, w.height - 60)))
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {   // the preset the map is, always shown (as emulators show their profile), and the presets to pick
                Text("Preset:")
                Menu(activePreset) {
                    ForEach(presets.indices, id: \.self) { i in
                        Toggle(presets[i].0, isOn: Binding(get: { presets[i].1 == map }, set: { _ in use(presets[i].1, "\(presets[i].0) map.") }))
                    }
                    Divider()
                    Button("Save as Preset…") { savePreset() }
                    Button("Show Presets Folder") {
                        try? FileManager.default.createDirectory(at: KeyMap.presetsFolder, withIntermediateDirectories: true)
                        NSWorkspace.shared.open(KeyMap.presetsFolder)
                    }
                }
                .fixedSize()
                Spacer()
            }
            .padding(.horizontal, 20).padding(.top, 16)
            ScrollView {   // one scroll; the cards in as many columns as the width takes, each column filled top down
                let split = KeyMapSheet.columns(size.width)
                HStack(alignment: .top, spacing: 16) {
                    ForEach(split.indices, id: \.self) { c in
                        VStack(spacing: 16) {
                            ForEach(split[c], id: \.self) { g in
                                if let group = KeyMap.groups.first(where: { $0.0 == g }) { card { header(group.0); ForEach(group.1, id: \.self) { t in row(t) } } } else { card { mouse } }
                            }
                        }
                        .frame(maxWidth: .infinity)
                    }
                }
                .padding(20)
            }
            Text(status.isEmpty ? footnote : status)
                .font(.footnote).foregroundStyle(status.isEmpty ? .secondary : .primary)
                .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 20).padding(.top, 8)
            HStack {
                Spacer()
                Button("Done") { stop(); dismiss() }.keyboardShortcut(.defaultAction).disabled(capturing != nil)
            }
            .padding(20)
        }
        .frame(width: size.width, height: size.height)
        .onAppear { map = KeyMap(saved); presets = KeyMap.presets() }
        .onDisappear { stop() }
    }

    private static let mouseCard = "Mouse and trackpad"
    /// The cards, one per group (the buttons first, as the controller is read) and the mouse's, in as many columns as the width takes, each filled top down
    /// until it holds about its share of the rows
    private static func columns(_ width: CGFloat) -> [[String]] {
        let first = ["Buttons", "Left stick", "Right stick", "D-pad"]
        let order = first.filter { t in KeyMap.groups.contains { $0.0 == t } } + KeyMap.groups.map(\.0).filter { !first.contains($0) } + [mouseCard]
        let rows = order.map { t in (KeyMap.groups.first { $0.0 == t }?.1.count ?? 6) + 1 }   // the mouse's: three outputs, two speeds and a note
        let n = width >= 1032 ? 3 : width >= 696 ? 2 : 1, share = Double(rows.reduce(0, +)) / Double(n) * 1.1
        var cols: [[String]] = [[]], height = 0
        for (t, r) in zip(order, rows) {
            if !cols[cols.count - 1].isEmpty && Double(height + r) > share && cols.count < n { cols.append([]); height = 0 }
            cols[cols.count - 1].append(t); height += r
        }
        return cols
    }
    private var activePreset: String { presets.first { $0.1 == map }?.0 ?? "Custom" }   // as the settings window's Keyboard row names it

    /// What mouse movement moves: per output, as the keys are per button
    @ViewBuilder private var mouse: some View {
        header(KeyMapSheet.mouseCard)
        ForEach(0..<KeyMap.outs, id: \.self) { o in mouseRows(o) }
        Text("The mouse moves one output at a time: the one whose key is held, else the one set to Mouse. In the game a click captures the pointer; Esc lets it go.")
            .font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
    }

    private func header(_ title: String) -> some View { Text(title).font(.headline).padding(.top, 6) }
    /// A card of the grid: rows on a rounded background, as a grouped Form's sections look
    private func card<C: View>(@ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 8) { content() }
            .padding(14).frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color(nsColor: .controlBackgroundColor)))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Color(nsColor: .separatorColor)))
    }
    /// A row: its label at the left, its control at the right
    private func line<C: View>(_ label: String, @ViewBuilder _ control: () -> C) -> some View {
        HStack { Text(label); Spacer(minLength: 8); control() }
    }

    private var footnote: String { "Click a button, then press its key, or hold Shift, Control or Option and press a key; a mouse button counts as a key; right-click for No Key or Add Key…." }

    private func row(_ t: Int) -> some View {
        let keys = map.keys(t)
        return line(KeyMap.label(target: t)) {
            Button(capturing == t ? "Press a key…" : keys.isEmpty ? "None" : keys.map { KeyMap.label(key: $0) }.joined(separator: ", ")) {
                capturing == t ? stop() : start(t)
            }
            .frame(minWidth: 140, alignment: .trailing)
            .foregroundStyle(keys.isEmpty && capturing != t ? .secondary : .primary)
            .contextMenu {
                Button("No Key") { map.clear(t); save("\(KeyMap.label(target: t)) has no key.") }
                Button("Add Key…") { start(t, add: true) }
            }
        }
    }

    /// An output's row under Mouse and trackpad: its source (None, Mouse, or a key + Mouse: while it is held); then its speed and Invert Y
    @ViewBuilder private func mouseRows(_ o: Int) -> some View {
        let name = KeyMap.outputs[o]
        line(name) {
            Menu(capturing == KeyMap.srcSlot(o) ? "Press a key…" : KeyMap.label(source: map.mouseSrc[o])) {
                Button("None") { map.setSource(o, KeyMap.srcOff); save("\(name): no mouse.") }
                Button("Mouse") { map.setSource(o, KeyMap.srcMouse); save("\(name): Mouse; in the game a click captures the pointer.") }
                Button("Hold a Key or Button…") { start(KeyMap.srcSlot(o)) }
            }
            .frame(width: 150)
        }
        if map.mouseSrc[o] != KeyMap.srcOff {
            line("Speed") {
                HStack {
                    speedSlider($map.mouseSpeed[o], name)
                    Toggle("Invert Y", isOn: Binding(get: { map.mouseInvert[o] != 0 }, set: { map.mouseInvert[o] = $0 ? 1 : 0; save("\(name): Invert Y \($0 ? "on" : "off").") }))
                }
            }
        }
    }

    /// A speed slider (0.2 to 6, tenths) with its value beside it, saved when the drag ends
    private func speedSlider(_ value: Binding<Float>, _ what: String) -> some View {
        HStack {
            Slider(value: Binding(get: { Double(value.wrappedValue) }, set: { value.wrappedValue = Float(($0 * 10).rounded() / 10) }), in: 0.2...10) { editing in
                if !editing { save(String(format: "%@: mouse speed %.1f.", what, value.wrappedValue)) }
            }
            .frame(width: 110)
            Text(String(format: "%.1f", value.wrappedValue)).monospacedDigit().frame(width: 30)
        }
    }

    private func use(_ m: KeyMap, _ note: String) { stop(); map = m; save(note) }

    /// The map as a keymap file in the presets folder, under a name the player types
    private func savePreset() {
        stop()
        let a = NSAlert()
        a.messageText = "Save as Preset"; a.informativeText = "A keymap file in the presets folder, for every game. A preset of the same name is replaced."
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24)); f.placeholderString = "Name"
        a.accessoryView = f; a.addButton(withTitle: "Save"); a.addButton(withTitle: "Cancel")
        a.window.initialFirstResponder = f
        guard a.runModal() == .alertFirstButtonReturn else { return }
        let typed = f.stringValue.trimmingCharacters(in: .whitespaces)
        guard !typed.isEmpty else { return }
        if let path = map.savePreset(typed) { status = "Saved as \(typed) (\((path as NSString).lastPathComponent))."; presets = KeyMap.presets() }
        else { status = "Could not save the preset." }
    }

    /// The map as launcher.DM_KEYMAP: removed while it is the default, so the default of a later build applies
    private func save(_ note: String) {
        saved = map == .standard ? "" : map.string
        status = note
    }

    private func start(_ t: Int, add: Bool = false) {
        stop()
        capturing = t; adding = add
        status = t <= KeyMap.srcSlot(0) ? "Press the key to hold for the mouse to move the \(KeyMap.outputs[KeyMap.srcSlot(0) - t]) (Esc stops)." : "Press \(add ? "another" : "a") key for \(KeyMap.label(target: t)) (Esc stops)."
        var pending: Int?   // a modifier pressed: bound alone if let go before a key
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged, .leftMouseDown, .rightMouseDown, .otherMouseDown]) { e in
            guard let t = capturing else { return e }
            let f = e.modifierFlags
            if e.type == .leftMouseDown || e.type == .rightMouseDown || e.type == .otherMouseDown {   // a mouse button is an input as a key is (the click that started this has ended)
                pending = nil
                let b = Int(e.buttonNumber)
                if let k = KeyMap.mouseKey(b) { bind(t, KeyMap.slot(k, f)) } else { NSSound.beep() }
                return nil
            }
            let code = Int(e.keyCode)
            if e.type == .flagsChanged {
                // a Shift, Control or Option key: down, it may start a chord; up with no key between, it is the binding. ⌘, Caps Lock and fn are not bindable
                // that very key, left or right: the device-dependent bits (NX_DEVICE*KEYMASK), as keyboard_mac.h's kb_modifier_down
                let bits: [Int: UInt] = [56: 0x2, 60: 0x4, 59: 0x1, 62: 0x2000, 58: 0x20, 61: 0x40]
                guard let bit = bits[code] else { return e }
                let down = f.rawValue & bit != 0
                if down && t <= KeyMap.srcSlot(0) { bind(t, code) }   // an output's mouse key takes a modifier at once
                else if down { pending = code } else if pending == code { pending = nil; bind(t, code) }
                return nil
            }
            pending = nil
            if e.isARepeat { return nil }
            if f.contains(.command) { return e }   // ⌘ shortcuts stay the menus'
            if code == 53 && !f.contains(.shift) && !f.contains(.control) && !f.contains(.option) { stop(); status = ""; return nil }   // Esc: no change
            let slot = KeyMap.slot(code, f)
            if KeyMap.bindable(slot) { bind(t, slot) } else { NSSound.beep() }
            return nil
        }
    }

    private func bind(_ t: Int, _ code: Int, add: Bool? = nil) {
        if t <= KeyMap.srcSlot(0) {   // an output's mouse key: any key, which keeps its own button too
            let o = KeyMap.srcSlot(0) - t, k = Int32(code % KeyMap.keyCount)
            map.setSource(o, k); stop()
            save("\(KeyMap.outputs[o]): \(KeyMap.label(source: k)), while it is held.")
            return
        }
        let also = add ?? adding
        let was = map.bind(t, code, add: also)
        stop()
        let key = KeyMap.label(key: code), name = KeyMap.label(target: t)
        save(was.map { "\(key) is \(name)\(also ? " too" : "") now; \(KeyMap.label(target: $0)) \(map.keys($0).isEmpty ? "has no key" : "keeps its other keys")." } ?? "\(key) is \(name)\(also ? " too" : "").")
    }

    private func stop() {
        if let m = monitor { NSEvent.removeMonitor(m) }
        monitor = nil; capturing = nil; adding = false
    }
}
