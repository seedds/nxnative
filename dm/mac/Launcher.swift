// Mac app launcher: the settings window shown before the game starts, in the launcher's own process (launcher.c calls dm_mac_launcher before it starts the host). Its settings come from the
// app's Resources/dm/launcher.json (dm/games/settings.py with SETTINGS_PLATFORM=mac); the model and the rows are the iOS launcher's (dm/ios/Sources/LauncherSettings.swift, LauncherRow.swift),
// and so is the store: launcher.<VAR> in the app's preferences, which launcher.c reads when the window is not shown (docs/frontends.md, Saved choices). Basic settings (level basic) come
// first, the others under Advanced settings, each in the list's sections. Play puts the settings into this process's environment, which the host inherits; Quit and closing the window end the
// app without the game. "Show this window at launch" off: no window unless Option is held at launch, and launcher.c applies the saved choices. The window never takes focus by itself: an app
// opened in the background (open -g) keeps its window behind until the player switches to it
import AppKit
import AVFAudio
import CoreAudio
import QuartzCore
import SwiftUI

private func info(_ key: String) -> String? { Bundle.main.object(forInfoDictionaryKey: key) as? String }

struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    let done: (Bool) -> Void
    let fit: (CGFloat) -> Void
    @AppStorage("window.show") private var show = true
    @AppStorage("window.advanced") private var advanced = false
    @AppStorage("launcher.DM_MIC_DEVICE") private var microphoneDeviceUID = ""
    @AppStorage("launcher.DM_MIC_GAIN") private var microphoneGain = "4"
    @State private var microphonePermission = AVAudioApplication.shared.recordPermission
    @State private var requestingMicrophonePermission = false
    @State private var launchAfterMicrophonePermission = false
    @State private var microphoneDevices = MicrophoneInputDevices.list()

    private var hasAudioInput: Bool { Bundle.main.object(forInfoDictionaryKey: "DMUsesAudioInput") as? Bool == true }
    private var microphonePermissionLabel: String {
        switch microphonePermission {
        case .granted: "Allowed"
        case .denied: "Denied"
        case .undetermined: "Not asked"
        @unknown default: "Unknown"
        }
    }
    private var microphoneGainValue: Double { min(max(Double(microphoneGain) ?? 4.0, 1.0), 8.0) }
    private var microphoneGainSlider: Binding<Double> {
        Binding(get: { microphoneGainValue }, set: {
            let value = Int($0.rounded())
            microphoneGain = String(value)
            if value == 4 { UserDefaults.standard.removeObject(forKey: "launcher.DM_MIC_GAIN") }
        })
    }
    private var microphoneDeviceSelection: Binding<String> {
        Binding(get: { microphoneDeviceUID }, set: {
            microphoneDeviceUID = $0
            if $0.isEmpty { UserDefaults.standard.removeObject(forKey: "launcher.DM_MIC_DEVICE") }
        })
    }
    private var environment: [String: String] { ProcessInfo.processInfo.environment }
    private var environmentKeys: Set<String> {
        Set(((environment["DM_ENV_FILE_KEYS"] ?? "") + " " + (environment["DM_ENV_LAUNCH_KEYS"] ?? "")).split(separator: " ").map(String.init))
    }
    private func fixedMicrophoneSetting(_ variable: String) -> String? {
        guard environmentKeys.contains(variable) || environment[variable] != nil else { return nil }
        return environment[variable] ?? ""
    }
    private func microphoneDeviceLabel(_ uid: String) -> String {
        if uid.isEmpty || uid == "default" { return "Default (system)" }
        return microphoneDevices.first(where: { $0.uid == uid })?.name ?? "Unavailable device (system default fallback)"
    }

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
                if hasAudioInput {
                    Section {
                        if let fixed = fixedMicrophoneSetting("DM_MIC_DEVICE") {
                            LabeledContent("Input device", value: microphoneDeviceLabel(fixed) + " (environment)")
                        } else {
                            Picker("Input device", selection: microphoneDeviceSelection) {
                                Text("Default (system)").tag("")
                                if !microphoneDeviceUID.isEmpty && microphoneDeviceUID != "default" && !microphoneDevices.contains(where: { $0.uid == microphoneDeviceUID }) {
                                    Text("Unavailable saved device").tag(microphoneDeviceUID)
                                }
                                ForEach(microphoneDevices) { device in Text(device.name).tag(device.uid) }
                            }
                        }
                        if let fixed = fixedMicrophoneSetting("DM_MIC_GAIN") {
                            LabeledContent("Microphone gain", value: "\(fixed)x (environment)")
                        } else {
                            HStack {
                                Text("Microphone gain")
                                Slider(value: microphoneGainSlider, in: 1...8, step: 1)
                                Text("\(Int(microphoneGainValue.rounded()))×").monospacedDigit().frame(width: 32, alignment: .trailing)
                            }
                        }
                        LabeledContent("Access", value: microphonePermissionLabel)
                    } footer: {
                        Text(microphonePermission == .denied
                             ? "Access is denied. Change it in System Settings > Privacy & Security > Microphone."
                             : "Access is requested when the launcher opens. If the selected device is unavailable, capture falls back to the system default.")
                    }
                }
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
                Button(requestingMicrophonePermission ? "Requesting Microphone Access…" : "Play") { play() }
                    .disabled(requestingMicrophonePermission)
                    .keyboardShortcut(.defaultAction)
            }
            .padding(20)
        }
        .frame(minWidth: 520, idealWidth: 560)
        .onAppear { refreshMicrophoneState(); requestMicrophonePermissionIfNeeded() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            refreshMicrophoneState()
            requestMicrophonePermissionIfNeeded()
        }
    }

    private func play() {
        guard hasAudioInput, microphonePermission == .undetermined else { done(true); return }
        requestMicrophonePermissionIfNeeded(playWhenFinished: true)
    }

    private func refreshMicrophoneState() {
        microphonePermission = AVAudioApplication.shared.recordPermission
        microphoneDevices = MicrophoneInputDevices.list()
    }

    private func requestMicrophonePermissionIfNeeded(playWhenFinished: Bool = false) {
        if playWhenFinished {
            guard microphonePermission == .undetermined else { done(true); return }
            launchAfterMicrophonePermission = true
        }
        guard hasAudioInput, microphonePermission == .undetermined, !requestingMicrophonePermission else { return }
        requestingMicrophonePermission = true
        AVAudioApplication.requestRecordPermission { _ in
            DispatchQueue.main.async {
                microphonePermission = AVAudioApplication.shared.recordPermission
                requestingMicrophonePermission = false
                fputs("launcher: microphone permission \(microphonePermissionLabel.lowercased())\n", stderr)
                if launchAfterMicrophonePermission {
                    launchAfterMicrophonePermission = false
                    done(true)
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
            return MicrophoneInputDevice(uid: uid, name: name)
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
