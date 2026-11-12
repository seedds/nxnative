// Mac app launcher: the settings window shown before the game starts, in the launcher's own process (launcher.c calls dm_mac_launcher before it starts the host). Its settings come from the
// app's Resources/dm/launcher.json (dm/games/settings.py with SETTINGS_PLATFORM=mac); the model is the iOS launcher's (dm/ios/Sources/LauncherSettings.swift),
// and so is the store: launcher.<VAR> in the app's preferences, which launcher.c reads when the window is not shown (docs/frontends.md, Saved choices). Basic settings (level basic) come
// first, the others under Advanced settings, each in the list's sections. Play puts the settings into this process's environment, which the host inherits; closing the window or ⌘Q ends the
// app without the game. "Show this window at launch" off: no window unless Option is held at launch, and launcher.c applies the saved choices. The window never takes focus by itself: an app
// opened in the background (open -g) keeps its window behind until the player switches to it
import AppKit
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

    /// Blocks while the cloud's file comes (SaveSyncReport.waitMs at most): call it off the main thread. nil: no report (sync is then left to the host as before)
    static func report() -> SaveSyncReport? {
        // getenv, not ProcessInfo.environment: launcher.c sets DM_SAVE and applies the env files in this process before the window
        guard let save = getenv("DM_SAVE") else { fputs("launcher: save sync: no DM_SAVE\n", stderr); return nil }
        guard let id = info("DMTitleID"), !id.isEmpty else { fputs("launcher: save sync: no DMTitleID\n", stderr); return nil }
        // the folder as launcher.c will pass it (cloud_saves_folder): DM_SAVE_SYNC from the launch environment or an env file, else the window's choice; "0" = off
        let folder = getenv("DM_SAVE_SYNC").map { String(cString: $0) } ?? (UserDefaults.standard.string(forKey: "cloudSaves.folder") ?? "")
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
    static func withoutWindow() -> Bool {
        final class Box: @unchecked Sendable { let lock = NSLock(); var report: SaveSyncReport?; var waiting = false }
        let app = NSApplication.shared
        defer { app.setActivationPolicy(.prohibited) }
        while true {
            let box = Box(), sem = DispatchSemaphore(value: 0)
            DispatchQueue.global(qos: .userInitiated).async {
                let r = report()
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

/// The window. A header (the game's icon, name and version, and Play: Return) over the settings: the basic ones in one box, cloud saves and the startup choice in a
/// second, then a row that opens Advanced settings below it, in the list's sections. Closing the window or ⌘Q ends the app without the game.
///
/// The settings are laid out here, in boxes drawn as a grouped Form draws its sections, and not by a Form. A Form has no height of its own: the window took it from
/// the Form's scroll geometry, a pass or more after the change, and jumped to it; and a click cost the main thread a layout before anything was drawn (an M4 Pro, a
/// 60 Hz screen: 76 to 85 ms when Advanced settings opened, its rows being made then, 28 to 38 ms for one setting with it open). This layout is not lazy: its height
/// is exact in the pass that changes it, which is what the window follows (fit). The Advanced rows stay laid out while the section is closed, clipped to no height, so opening it is the window growing over rows that
/// are already there, and closing it the window closing over them before they go.
struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    let done: (Bool) -> Void
    let fit: (CGFloat) -> Void   // the height the content wants: the header and the settings, Advanced settings included while it is open
    @AppStorage("window.show") private var show = true
    @AppStorage("window.advanced") private var advanced = false
    @State private var checking = false
    @State private var checkingShown = false   // the check's status, shown only once it has taken a moment (a check without a cloud folder returns at once, and a status flashed for a frame reads as a glitch)
    @State private var check = 0   // which Play the running check is for: one the player went on from without it is dropped
    @State private var scrolled = false   // settings scrolled under the header
    @State private var closing = false   // Advanced settings is closing: its rows stay until the window has closed over them
    @State private var closes = 0   // which close the pending end of `closing` is for
    @State private var heights = Heights()
    @State private var position = ScrollPosition(edge: .top)

    struct Heights: Equatable { var header: CGFloat = 0, basic: CGFloat = 0, advanced: CGFloat = 0 }

    /// The game's icon (pack_app.py's AppIcon.icns, from the game's control data or --icon); nil when the app has none (a dump or a NUP without one)
    static let art: NSImage? = info("CFBundleIconFile").flatMap { Bundle.main.image(forResource: $0) }
    private var wanted: CGFloat { heights.header + heights.basic + (advanced ? heights.advanced : 0) }

    var body: some View {
        VStack(spacing: 0) {
            header
                .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { heights.header = $0 }
                .overlay(alignment: .bottom) { Divider().opacity(scrolled ? 1 : 0) }
            ScrollView {
                VStack(spacing: 0) {
                    basicSettings
                        .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { heights.basic = $0 }
                    advancedSettings
                        .fixedSize(horizontal: false, vertical: true)   // its own height, whatever it is clipped to
                        .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { heights.advanced = $0 }
                        .frame(height: advanced || closing ? nil : 0, alignment: .top)
                        .clipped()
                        .disabled(!advanced && !closing)   // out of the key view loop while it is out of sight
                        .allowsHitTesting(advanced)
                        .accessibilityHidden(!advanced)
                }
            }
            .scrollPosition($position)
            .scrollDisabled(closing)
            .scrollBounceBehavior(.basedOnSize)
            .onScrollGeometryChange(for: Bool.self) {
                $0.contentOffset.y > -$0.contentInsets.top + 1
            } action: { _, under in
                scrolled = under
            }
        }
        .frame(minWidth: 520, idealWidth: 560)
        .onChange(of: wanted, initial: true) {
            if heights.header > 0, heights.basic > 0 { fit(wanted) }
        }
        .onChange(of: advanced) { _, open in
            closes += 1
            closing = !open
            guard !open else { return }
            position.scrollTo(edge: .top)   // the window closes down to the basic settings, wherever the list was scrolled to
            let mine = closes
            DispatchQueue.main.asyncAfter(deadline: .now() + Launcher.settleTime) { if closes == mine { closing = false } }
        }
    }

    private var header: some View {
        VStack(spacing: 0) {
            GameIcon(art: Self.art, side: 96)
            Text(info("CFBundleName") ?? "Game").font(.system(.title, design: .rounded).weight(.bold)).lineLimit(2).multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, 14)
            Group {
                if checkingShown {
                    HStack(spacing: 6) {
                        ProgressView().controlSize(.small)
                        Text("Checking the cloud save…").foregroundStyle(.secondary)
                        Button("Play Without It") { checking = false; checkingShown = false; SaveCheck.apply("offline"); done(true) }
                            .buttonStyle(.link)
                    }
                    .lineLimit(1)
                    .transition(.opacity)
                } else {
                    Text("Version \(info("CFBundleShortVersionString") ?? "1.0")").foregroundStyle(.secondary)
                        .transition(.opacity)
                }
            }
            .font(.callout)
            .padding(.top, 3)
            Button { start() } label: {
                Label("Play", systemImage: "play.fill").font(.system(.title3, design: .rounded).weight(.semibold)).padding(.horizontal, 30)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.extraLarge)
            .keyboardShortcut(.defaultAction)
            .disabled(checking)
            .padding(.top, 16)
        }
        .animation(.easeOut(duration: 0.15), value: checkingShown)
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 24).padding(.top, 2).padding(.bottom, 14)
    }

    private var basicSettings: some View {
        VStack(spacing: 12) {
            // The basic settings in one box, with no headings: a heading over one or two rows that name themselves is noise. The advanced ones keep the list's sections
            let basic = m.settings.filter { $0.basic && m.visible($0) }
            if !basic.isEmpty {
                Box { ForEach(basic) { SettingLine($0, m).equatable() } }
            }
            Box {
                CloudSavesRow()
                HStack(spacing: 12) {
                    RowLabel(icon: "arrow.up.forward.app") {
                        Text("Show this window at launch")
                        Text("With this off, hold Option (⌥) while opening the app to see it.").font(.footnote).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 8)
                    Toggle("Show this window at launch", isOn: $show).labelsHidden().toggleStyle(.switch).controlSize(.mini)
                }
            }
            Box(inset: false) {
                AdvancedRow(expanded: $advanced, changed: m.settings.filter { !$0.basic && !m.fixed.contains($0.env) && !$0.isDefault(m.values[$0.env] ?? $0.def) }.count)
            }
        }
        .padding(20)
    }

    private var advancedSettings: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(m.sections(basic: false).enumerated()), id: \.element) { i, section in
                let rows = m.rows(in: section, basic: false)
                if !rows.isEmpty {
                    // 38 pt from the box above to a heading, as a Form sets a headed section: the first is 18 below the basic settings' own 20
                    Text(section).font(.headline).padding(.leading, 10).padding(.top, i == 0 ? 18 : 38).padding(.bottom, 10)
                    Box { ForEach(rows) { SettingLine($0, m).equatable() } }
                }
            }
            Box {
                HStack(spacing: 12) {
                    RowLabel(icon: "arrow.counterclockwise") {
                        HStack(spacing: 6) {
                            Text("Reset to tested defaults")
                            InfoButton(title: "Reset to tested defaults", text: "Every setting goes back to what this game was tested with. Your choices are forgotten when you press Play.")
                        }
                    }
                    Spacer(minLength: 8)
                    Button("Reset All") { m.reset() }
                        .disabled(!m.changed)
                }
            }
            .padding(.top, 12)
        }
        .padding(.horizontal, 20).padding(.bottom, 24)
    }

    /// Play, once the cloud's save is compared with this Mac's: a save that changed on both sides is the player's pick, not the newer file's (SaveCheck)
    private func start() {
        check += 1; checking = true
        let mine = check
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { if checking, check == mine { checkingShown = true } }
        DispatchQueue.global(qos: .userInitiated).async {
            let r = SaveCheck.report()
            DispatchQueue.main.async {
                guard checking, check == mine else { return }   // the player played without it
                checking = false; checkingShown = false
                switch SaveCheck.ask(r, cancel: "Cancel") {
                case .play(let choice): SaveCheck.apply(choice); done(true)
                case .retry: start()
                case .cancel: break   // back to the window, nothing chosen
                }
            }
        }
    }
}

/// Rows in a rounded box with hairlines between them, drawn as a grouped Form draws a section (measured on macOS 27: the fill is 3 % of the label colour, the corner
/// 12 pt, the rows and the hairlines inset 10 pt). `inset: false` for a row that fills the box itself (a row that is one button)
private struct Box<Rows: View>: View {
    var inset = true
    @ViewBuilder let rows: Rows

    var body: some View {
        VStack(spacing: 0) {
            Group(subviews: rows) { subviews in
                ForEach(Array(subviews.enumerated()), id: \.element.id) { i, row in
                    if i > 0 { Divider().padding(.horizontal, 10) }
                    row.padding(.horizontal, inset ? 10 : 0).padding(.vertical, inset ? 8 : 0).frame(minHeight: inset ? 37 : 0)
                }
            }
        }
        .background(Color.primary.opacity(0.03), in: RoundedRectangle(cornerRadius: 12))
    }
}

/// A row's label: its symbol in the first line, the lines beside it. The symbol takes the accent colour while the row's setting differs from its tested default
struct RowLabel<Lines: View>: View {
    let icon: String
    var changed = false
    var changedHelp = ""
    @ViewBuilder let lines: Lines

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: icon).foregroundStyle(changed ? AnyShapeStyle(.tint) : AnyShapeStyle(.secondary)).frame(width: 20)
                .help(changed ? changedHelp : "")
                .accessibilityLabel(changed ? changedHelp : "").accessibilityHidden(!changed)
            VStack(alignment: .leading, spacing: 4) { lines }
        }
    }
}

/// One setting as a row: its symbol and name left, its control right (a switch; two or three short options side by side, a longer choice as a menu; a slider with its
/// value). What the setting does is behind an info button, not under every name. A setting that differs from its tested default is marked without moving anything: its
/// symbol takes the accent colour (the accent colour is kept for what can be clicked and for this mark), and where the default is not plainly the other value (a
/// slider, a choice of more than two) a Reset link stands after its name. A row fixed by an env file or by the launch environment says so, and its control is disabled.
/// The row is drawn from its own value, not from the model: it is Equatable, so a change of one setting redraws that row. Observing the model, every row was redrawn for
/// any change: 18 to 35 ms on the main thread with Advanced settings open, which a control's own animation showed as a stutter; 12 ms and less now.
private struct SettingLine: View, Equatable {
    let s: Setting
    let value: String
    let fixedBy: String?   // what fixes the row ("Set at launch", "Set by an env file"); nil: the player's to change
    let handheld: Bool     // the chosen console mode, for the render scale's size in pixels
    let set: (String) -> Void

    init(_ s: Setting, _ m: LauncherModel) {
        self.s = s
        value = m.values[s.env] ?? s.def
        fixedBy = m.fixed.contains(s.env) ? (m.fromLaunch.contains(s.env) ? "Set at launch" : "Set by an env file") : nil
        handheld = s.base != nil && m.handheld   // only a row that shows a size in pixels follows the console mode
        set = { [weak m] in m?.values[s.env] = $0 }
    }
    static func == (a: SettingLine, b: SettingLine) -> Bool { a.s.env == b.s.env && a.value == b.value && a.fixedBy == b.fixedBy && a.handheld == b.handheld }

    private var changed: Bool { fixedBy == nil && !s.isDefault(value) }
    // A Reset link only where the tested default is not plainly the other value: a slider, a choice of more than two. A switch or a choice of two is reset by itself
    private var resets: Bool { s.kind == "range" || (s.kind == "choice" && (s.options?.count ?? 0) > 2) }
    // Side by side only when every label fits its share of the control's width: two languages are two options too, and "American English" does not
    private var segmented: Bool {
        guard let o = s.options, (2...3).contains(o.count) else { return false }
        let room = SlidingChoice.width / CGFloat(o.count) - 16, font = NSFont.systemFont(ofSize: NSFont.systemFontSize)
        return o.allSatisfy { ($0[1] as NSString).size(withAttributes: [.font: font]).width <= room }
    }
    private var text: Binding<String> { Binding(get: { value }, set: set) }
    // A stepped slider is drawn with a tick for every step (20 dots under a 5 % step), so the slider is continuous and the value snaps to the step here
    private var number: Binding<Double> {
        Binding(get: { Double(value) ?? s.max ?? 1 },
                set: { let step = s.step ?? 0.05, lo = s.min ?? 0; set(String(format: "%.2f", lo + (($0 - lo) / step).rounded() * step)) })
    }
    private var defaultLabel: String {
        switch s.kind {
        case "choice": return s.options?.first { $0[0] == s.def }?[1] ?? s.def
        case "range": return LauncherModel.rangeLabel(s, Double(s.def) ?? 0, handheld: handheld)
        default: return s.def == s.onValue ? "On" : "Off"
        }
    }

    var body: some View {
        HStack(spacing: 12) {
            RowLabel(icon: Symbols.rows[s.env] ?? "gearshape", changed: changed, changedHelp: "Changed. Tested default: \(defaultLabel)") {
                HStack(spacing: 6) {
                    Text(s.label).opacity(fixedBy == nil ? 1 : 0.5)
                    if !s.note.isEmpty { InfoButton(title: s.label, text: s.note) }
                    if changed, resets {
                        Button("Reset") { set(s.def) }
                            .buttonStyle(.link).font(.callout)
                            .help("Tested default: \(defaultLabel)")
                            .accessibilityLabel("Reset \(s.label) to its tested default, \(defaultLabel)")
                            .transition(.opacity)
                    }
                }
                if let fixedBy { Text(fixedBy).font(.footnote).foregroundStyle(.secondary) }
            }
            Spacer(minLength: 8)
            control.disabled(fixedBy != nil)
        }
        .animation(.easeOut(duration: 0.15), value: changed)
    }

    @ViewBuilder private var control: some View {
        switch s.kind {
        case "choice":
            if segmented {
                SlidingChoice(title: s.label, options: s.options ?? [], value: value, set: set)
            } else {
                Picker(s.label, selection: text) {
                    ForEach(s.options ?? [], id: \.[0]) { Text($0[1]).tag($0[0]) }
                }
                .labelsHidden().pickerStyle(.menu).buttonStyle(.borderless).fixedSize()   // borderless: the value and its chevrons, as a Form draws a menu
            }
        case "range":
            let label = LauncherModel.rangeLabel(s, number.wrappedValue, handheld: handheld)
            HStack(spacing: 10) {
                Slider(value: number, in: (s.min ?? 0)...(s.max ?? 1))
                    .frame(width: s.base == nil ? 150 : 120)   // room for the size in pixels beside it
                    .accessibilityLabel(s.label)
                    .accessibilityValue(label)
                // At its own width or 44 pt, whichever is more, and not less: a frame with only a minimum is shrunk by the row before its Spacer is,
                // and the size in pixels then ran over the slider
                Text(label).monospacedDigit().foregroundStyle(.secondary).lineLimit(1).frame(minWidth: 44, alignment: .trailing).fixedSize()
            }
        default:
            Toggle(s.label, isOn: Binding(get: { value == s.onValue }, set: { set($0 ? s.onValue : s.offValue) }))
                .labelsHidden().toggleStyle(.switch).controlSize(.mini)   // the size of a Form's switch
        }
    }
}

/// What a setting does, on demand: a small info button after its name. A click shows the explanation in a popover, a second click (or one anywhere else, or Escape)
/// puts it away; it is also the button's help tag
struct InfoButton: View {
    let title, text: String
    @State private var shown = false

    var body: some View {
        Button { shown.toggle() } label: {
            Image(systemName: "info.circle").foregroundStyle(.secondary)
        }
        .buttonStyle(.plain)
        .help(text)
        .accessibilityLabel("About \(title)")
        .background(InfoPopover(text: text, shown: $shown))
    }
}

/// The info button's popover: an NSPopover of a wrapping label, which this code opens and closes, with a short fade in and out.
/// - Not SwiftUI's .popover: it hosts a SwiftUI view and animates it in, and the first one cost the main thread 50 ms before it showed, which the player saw as its
///   animation stuttering. A label costs nothing to lay out, and a 150 ms fade of the popover's window is the window server's work.
/// - Not a transient popover: the system closes one on any click outside it, the click on its own button included, and the button's action then cannot tell that
///   click from one meant to open it (told apart by time, a slow click opened it again). Here a click on the button is left to the button, and every other click,
///   Escape, a scroll or the app going to the background closes the popover.
private struct InfoPopover: NSViewRepresentable {
    let text: String
    @Binding var shown: Bool

    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSView { NSView() }
    func updateNSView(_ anchor: NSView, context: Context) {
        context.coordinator.didClose = { shown = false }
        if shown { context.coordinator.open(text, at: anchor) } else { context.coordinator.close() }
    }
    static func dismantleNSView(_ anchor: NSView, coordinator: Coordinator) { coordinator.close(fade: false) }

    final class Coordinator: NSObject {
        var didClose: () -> Void = {}
        private var popover: NSPopover?
        private var monitor: Any?
        private var observer: NSObjectProtocol?

        func open(_ text: String, at anchor: NSView) {
            guard popover == nil, anchor.window != nil else { return }
            let label = NSTextField(wrappingLabelWithString: text)
            label.font = .preferredFont(forTextStyle: .callout)
            label.isSelectable = false
            label.translatesAutoresizingMaskIntoConstraints = false
            let box = NSView()
            box.addSubview(label)
            NSLayoutConstraint.activate([label.leadingAnchor.constraint(equalTo: box.leadingAnchor, constant: 14), label.trailingAnchor.constraint(equalTo: box.trailingAnchor, constant: -14),
                                         label.topAnchor.constraint(equalTo: box.topAnchor, constant: 12), label.bottomAnchor.constraint(equalTo: box.bottomAnchor, constant: -12),
                                         label.widthAnchor.constraint(equalToConstant: min(260, ceil(label.intrinsicContentSize.width)))])   // a short explanation at its own width
            let controller = NSViewController()
            controller.view = box
            let popover = NSPopover()
            popover.contentViewController = controller
            popover.behavior = .applicationDefined
            popover.animates = false
            self.popover = popover
            popover.show(relativeTo: anchor.bounds, of: anchor, preferredEdge: anchor.isFlipped ? .maxY : .minY)   // below the button
            if let bubble = box.window {
                bubble.alphaValue = 0
                NSAnimationContext.runAnimationGroup { $0.duration = 0.15; $0.timingFunction = CAMediaTimingFunction(name: .easeOut); bubble.animator().alphaValue = 1 }
            }
            monitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown, .keyDown, .scrollWheel]) { [weak self, weak anchor, weak box] event in
                guard let self, self.popover != nil else { return event }
                if event.type == .keyDown {
                    guard event.keyCode == 53 else { return event }   // Escape
                    self.close(); return nil
                }
                if let bubble = box?.window, event.window === bubble { return event }   // a click in the popover itself
                if event.type == .leftMouseDown, let anchor, event.window === anchor.window,
                   anchor.convert(anchor.bounds, to: nil).insetBy(dx: -3, dy: -3).contains(event.locationInWindow) { return event }   // on the button: its action closes it
                self.close()
                return event
            }
            observer = NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in self?.close(fade: false) }
        }

        func close(fade: Bool = true) {
            guard let popover else { return }
            self.popover = nil
            if let monitor { NSEvent.removeMonitor(monitor) }
            if let observer { NotificationCenter.default.removeObserver(observer) }
            monitor = nil; observer = nil
            // Out a little faster than in, and at an even rate: on the default curve an 80 ms fade was at 13 % in its first frame, which read as no fade at all
            if fade, let bubble = popover.contentViewController?.view.window {
                NSAnimationContext.runAnimationGroup({ $0.duration = 0.13; $0.timingFunction = CAMediaTimingFunction(name: .linear); bubble.animator().alphaValue = 0 }, completionHandler: { popover.close() })
            } else {
                popover.close()
            }
            didClose()
        }
    }
}

/// A choice of two or three short options as one control: the options side by side in a track, every such control the same width, and a thumb in the accent colour that
/// slides to the chosen one (AppKit's segmented control, which this stands in for, changes its selection at once, and is as wide as its labels: no two alike in a
/// column of them). The thumb goes to the option under the pointer as the button goes down and follows it while it is held, on a spring; the release chooses. The
/// labels are drawn twice, in the text colour and in white, the white ones masked to the thumb, so a label changes colour exactly as the thumb passes over it.
/// Left and right arrows move it when it has the keyboard focus, and VoiceOver adjusts it
private struct SlidingChoice: View {
    let title: String
    let options: [[String]]   // [value, label]
    let value: String
    let set: (String) -> Void
    @State private var held: Int?   // the option under the pointer while the button is down
    @Environment(\.isEnabled) private var enabled
    @Environment(\.controlActiveState) private var active
    @Environment(\.accessibilityReduceMotion) private var still

    static let width: CGFloat = 200, height: CGFloat = 24

    var body: some View {
        let count = max(options.count, 1), cell = Self.width / CGFloat(count)
        let chosen = options.firstIndex { $0[0] == value } ?? 0, at = held ?? chosen
        let key = active != .inactive   // a control in a window that is not the key window gives up its colour, as AppKit's do
        let thumb = RoundedRectangle(cornerRadius: 5)
            .frame(width: cell - 4, height: Self.height - 4)
            .scaleEffect(held == nil ? 1 : 0.96)
            .offset(x: (CGFloat(at) - CGFloat(count - 1) / 2) * cell)
        ZStack {
            RoundedRectangle(cornerRadius: 7).fill(Color.primary.opacity(0.07))
            thumb.foregroundStyle(key ? AnyShapeStyle(.tint) : AnyShapeStyle(Color.primary.opacity(0.18)))
            labels(cell).foregroundStyle(.primary)
            if key { labels(cell).foregroundStyle(.white).mask(thumb) }
        }
        .frame(width: Self.width, height: Self.height)
        .animation(still ? nil : .spring(duration: 0.25, bounce: 0), value: at)
        .animation(.easeOut(duration: 0.1), value: held == nil)
        .opacity(enabled ? 1 : 0.5)
        .contentShape(.rect)
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { held = min(max(Int($0.location.x / cell), 0), count - 1) }
            .onEnded { _ in
                if let held, options.indices.contains(held), held != chosen { set(options[held][0]) }
                held = nil
            })
        .focusable(interactions: .activate)   // the keyboard focus as a button takes it (Full Keyboard Access), not by a click: a click left a focus ring round it
        .contentShape(.focusEffect, RoundedRectangle(cornerRadius: 7))
        .onMoveCommand { direction in
            let next = chosen + (direction == .right ? 1 : direction == .left ? -1 : 0)
            if next != chosen, options.indices.contains(next) { set(options[next][0]) }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(title)
        .accessibilityValue(options.indices.contains(chosen) ? options[chosen][1] : "")
        .accessibilityAdjustableAction { direction in
            let next = chosen + (direction == .increment ? 1 : -1)
            if options.indices.contains(next) { set(options[next][0]) }
        }
    }

    private func labels(_ cell: CGFloat) -> some View {
        HStack(spacing: 0) {
            ForEach(options, id: \.[0]) { Text($0[1]).lineLimit(1).frame(width: cell) }
        }
    }
}

/// The symbol before each setting's row (an SF Symbol's name), by the variable the row sets. A row of dm/games/settings.tsv that a Mac app can show has its
/// symbol here, and nothing else has: test_launcher_storage.py compares the two, so a new setting is not drawn with the fallback, a gear
enum Symbols {
    static let rows: [String: String] = [
        "DM_FPS_TARGET": "speedometer", "DM_DYNAMIC_RESOLUTION": "square.resize", "DM_RENDER_SCALE": "arrow.up.left.and.arrow.down.right", "DM_OPMODE": "tv",
        "DM_PRESENT_UPSCALE": "square.arrowtriangle.4.outward", "DM_UPSCALER": "wand.and.sparkles", "DM_LANG": "globe", "DM_FULLSCREEN": "macwindow",
        "DM_SCREEN_ASPECT": "aspectratio", "DM_RUMBLE": "waveform", "DM_RUMBLE_GAIN": "dial.medium",
        "DM_RUMBLE_MIN": "line.3.horizontal.decrease", "DM_PAD_PRESS_TO_JOIN": "person.badge.plus", "DM_SW2": "gamecontroller", "DM_PRESENT_SNAP": "square.3.layers.3d",
        "DM_PSO_MTL4": "bolt", "DM_PSO_PREBUILD": "hourglass", "DM_NVN_ENC_THREAD": "arrow.triangle.branch", "DM_FPS_HOLD": "timer", "DM_METAL_HUD": "chart.xyaxis.line",
    ]
}

/// The game's icon as a rounded tile; a controller glyph when the app has no icon
private struct GameIcon: View {
    let art: NSImage?
    let side: CGFloat

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: side * 0.225, style: .continuous)   // the corner of a macOS app icon
        Group {
            if let art {
                Image(nsImage: art).resizable().interpolation(.high).scaledToFill()
            } else {
                ZStack {
                    Rectangle().fill(.quaternary)
                    Image(systemName: "gamecontroller.fill").font(.system(size: side * 0.42)).foregroundStyle(.secondary)
                }
            }
        }
        .frame(width: side, height: side)
        .clipShape(shape)
        .overlay(shape.strokeBorder(.primary.opacity(0.1), lineWidth: 0.5))
        .shadow(color: .black.opacity(0.22), radius: side / 13, y: side / 40)
        .accessibilityHidden(true)
    }
}

/// The row that opens and closes Advanced settings: the whole box is the control, and it says how many advanced settings differ from the tested defaults. Its chevron
/// stands in the rows' symbol column and turns as the section opens
private struct AdvancedRow: View {
    @Binding var expanded: Bool
    let changed: Int

    var body: some View {
        Button { expanded.toggle() } label: {
            HStack(spacing: 10) {
                Image(systemName: "chevron.right")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(.secondary)
                    .rotationEffect(.degrees(expanded ? 90 : 0))
                    .frame(width: 20)
                Text("Advanced settings")
                Spacer(minLength: 8)
                if changed > 0 { Text(changed == 1 ? "1 changed" : "\(changed) changed").foregroundStyle(.secondary) }
            }
            .padding(.horizontal, 10).padding(.vertical, 8).frame(minHeight: 37)
            .contentShape(.rect)
        }
        .buttonStyle(PressRow())
        .animation(.easeOut(duration: 0.18), value: expanded)
        .accessibilityValue(expanded ? "Expanded" : "Collapsed")
    }
}

/// A plain row that dims while pressed: feedback on the press, not only on the release
private struct PressRow: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.opacity(configuration.isPressed ? 0.55 : 1)
    }
}

/// Cloud saves: a folder the save is synced through (dm/host/save_sync.h), the one the iPhone app picks in its launcher's Cloud saves section. It is kept in this app's
/// preferences as cloudSaves.folder, which launcher.c puts into DM_SAVE_SYNC before the host starts; a DM_SAVE_SYNC an env file or the launch environment set wins, and is shown here.
/// One row: Off or the folder under its name, what it does behind the info button
struct CloudSavesRow: View {
    @AppStorage("cloudSaves.folder") private var folder = ""
    private let fixed: String? = {
        let env = ProcessInfo.processInfo.environment
        let keys = ((env["DM_ENV_FILE_KEYS"] ?? "") + " " + (env["DM_ENV_LAUNCH_KEYS"] ?? "")).split(separator: " ")
        return keys.contains("DM_SAVE_SYNC") || env["DM_SAVE_SYNC"] != nil ? env["DM_SAVE_SYNC"] : nil
    }()

    var body: some View {
        HStack(spacing: 12) {
            RowLabel(icon: "icloud") {
                HStack(spacing: 6) {
                    Text("Cloud saves")
                    InfoButton(title: "Cloud saves", text: "Shares this game's save with the iPhone app through a folder in iCloud Drive: choose the folder the iPhone app uses. A save that is replaced is kept in the save's backups folder.")
                }
                Group {
                    if let fixed {
                        Text("\(Self.shown(fixed)) (env.txt)").help(fixed)
                    } else if folder.isEmpty {
                        Text("Off")
                    } else {
                        Text(Self.shown(folder)).help(folder)
                    }
                }
                .font(.footnote).foregroundStyle(.secondary).lineLimit(1).truncationMode(.head)
            }
            Spacer(minLength: 8)
            if fixed == nil {
                if !folder.isEmpty { Button("Turn Off") { folder = "" } }
                Button(folder.isEmpty ? "Choose Folder…" : "Change…", action: choose)
            }
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
    var host: NSView?   // the settings (an NSHostingView), in the window's content view at its top edge and at its own height (spring)
    var play = false
    var placed = false   // the window has its measured height and its place on the screen; until then it is transparent (place)

    init(_ model: LauncherModel) { self.model = model }

    func applicationDidFinishLaunching(_ n: Notification) {
        let name = info("CFBundleName") ?? "Game"
        let menu = NSMenu(), item = NSMenuItem(), app = NSMenu()
        app.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        app.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = app; menu.addItem(item); NSApp.mainMenu = menu
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 660), styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView], backing: .buffered, defer: false)   // not resizable: the height follows the settings shown (fit), which a dragged edge only fought
        w.title = name; w.isReleasedWhenClosed = false; w.delegate = self
        w.titleVisibility = .hidden; w.titlebarAppearsTransparent = true   // the header names the game; the title stays for the Window menu, Mission Control and VoiceOver
        w.alphaValue = 0   // until place(): the content reports its height a pass after the window is ordered in, and the window showed at this first size, then jumped
        window = w
        if let screen = w.screen ?? NSScreen.main {
            w.setFrameTopLeftPoint(NSPoint(x: screen.visibleFrame.midX - w.frame.width / 2, y: screen.visibleFrame.maxY))
        }
        let host = SettingsHost(rootView: LauncherView(m: model, done: { [weak self] in self?.finish($0) }, fit: { [weak self] in self?.fit($0) }))
        host.sizingOptions = []   // the window's height is the content's own (fit), not the hosting view's ideal size: the settings scroll when the screen is shorter
        // The settings are not the window's content view but a view in it, held at the top edge: while the window's height moves, theirs is the height it is moving
        // to (or from), so they are laid out once per move and the window only uncovers or covers them. As the content view they were laid out at every step, 35 ms
        // each on an M4 Pro: 28 steps a second on a 60 Hz screen. Now a step is the window's resize alone, 1.2 to 2 ms
        let content = TopDown(frame: w.contentLayoutRect)
        content.autoresizesSubviews = false
        host.frame = content.bounds
        content.addSubview(host)
        w.contentView = content
        self.host = host
        host.layoutSubtreeIfNeeded()
        // no activation and no ordering in front of another app's windows: LaunchServices activates an app opened from Finder or the Dock (applicationDidBecomeActive), open -g does not
        if NSApp.isActive { w.makeKeyAndOrderFront(nil) } else { w.orderBack(nil) }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in self?.place() }   // a content that reported nothing: the window as it is, not none
    }
    func applicationDidBecomeActive(_ n: Notification) { window?.makeKeyAndOrderFront(nil) }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {   // ⌘Q, or a logout or restart: the app ends without the game (exit 0, as Quit)
        fputs("launcher: quit without starting the game\n", stderr)
        return .terminateNow   // not terminateCancel, which a logout or restart takes as a refusal and stops
    }
    func windowWillClose(_ n: Notification) { finish(false) }

    /// The content's height (LauncherView.fit): the window takes it, its top edge where it is. The first time it is set at once and the window shown (place); after
    /// that the window moves to it on a spring
    func fit(_ height: CGFloat) {
        guard let w = window, let screen = w.screen else { return }
        let chrome = w.frame.height - w.contentLayoutRect.height   // the title bar: the content lies under it (fullSizeContentView), and its safe area keeps clear of it
        let wanted = LauncherWindowLayout.fitted(w.frame, height: height + chrome, visible: screen.visibleFrame).height
        if placed { spring(to: wanted) } else { setHeight(wanted); layOut(wanted); place() }
    }

    /// Once, when the content's height is known: the window goes where AppKit puts a panel (centred, above the middle) and fades in. Until then it sat at the top of the
    /// screen, where any height fits below it
    func place() {
        guard !placed, let w = window else { return }
        placed = true
        w.center()
        NSAnimationContext.runAnimationGroup { $0.duration = 0.12; w.animator().alphaValue = 1 }   // not a pop from nothing: it was ordered in transparent, so AppKit's own opening animation went unseen
    }

    // The window's height follows the content's on a critically damped spring: no overshoot, and it starts from the height and the speed the window has, so a second
    // click on Advanced settings while it moves turns it round instead of starting over or waiting. A timed curve can do neither. Reduce Motion: no spring, the height
    // is set. `response` is the spring's period (Apple's parameter); the window is within half a point of a 700 pt move after `settleTime`
    static let response = 0.25
    static let settleTime = 0.45
    private var link: CADisplayLink?
    private var goal: CGFloat = 0, c1: CGFloat = 0, c2: CGFloat = 0, began: CFTimeInterval = 0   // height(t) = goal + (c1 + c2 t) e^(-ωt)

    private func spring(to height: CGFloat) {
        guard let w = window, abs(height - (link == nil ? w.frame.height : goal)) > 0.5 else { return }
        if NSWorkspace.shared.accessibilityDisplayShouldReduceMotion { link?.invalidate(); link = nil; setHeight(height); layOut(height); return }
        if let host, height > host.frame.height { layOut(height) }   // growing: the settings at the height to come, which the window uncovers; shrinking: as they are, until it has covered them
        let omega = 2 * .pi / Self.response, now = CACurrentMediaTime()
        var x = w.frame.height, v: CGFloat = 0
        if link != nil { let t = now - began, e = exp(-omega * t); x = goal + (c1 + c2 * t) * e; v = (c2 - omega * (c1 + c2 * t)) * e }
        goal = height; c1 = x - height; c2 = v + omega * c1; began = now
        if link == nil, let view = w.contentView {
            link = view.displayLink(target: self, selector: #selector(step))   // the screen's refresh, whichever screen the window is on
            if let fps = w.screen?.maximumFramesPerSecond, fps > 60 { link?.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: Float(fps), preferred: Float(fps)) }   // a fast screen's own rate: the default is 60
            link?.add(to: .main, forMode: .common)
        }
    }
    @objc private func step(_ frame: CADisplayLink) {
        let omega = 2 * .pi / Self.response, t = max(0, frame.targetTimestamp - began), e = exp(-omega * t)
        let x = goal + (c1 + c2 * t) * e, v = (c2 - omega * (c1 + c2 * t)) * e
        if abs(x - goal) < 0.5, abs(v) < 20 { link?.invalidate(); link = nil; setHeight(goal); layOut(goal) } else { setHeight(x) }
    }
    /// The settings at a window height: its full width, from the top edge down
    private func layOut(_ height: CGFloat) {
        guard let w = window, let host, let content = w.contentView else { return }
        let size = NSSize(width: content.bounds.width, height: height)
        if host.frame.size != size || host.frame.origin != .zero { host.frame = NSRect(origin: .zero, size: size) }
    }
    private func setHeight(_ height: CGFloat) {
        guard let w = window else { return }
        let scale = w.backingScaleFactor, h = (height * scale).rounded() / scale
        w.setFrame(NSRect(x: w.frame.minX, y: w.frame.maxY - h, width: w.frame.width, height: h), display: true)
    }

    func finish(_ play: Bool) {
        guard let w = window else { return }
        window = nil; self.play = play
        link?.invalidate(); link = nil
        // The main thread blocks in launcher.c's waitpid once run() returns, so the window must leave the screen now: no fade-out (it runs on later run-loop passes),
        // and dmMacLauncher flushes the order-out (AppKit commits it at the end of a pass). Without both, the window stayed on screen, not responding, until the game ended
        w.animationBehavior = .none
        w.delegate = nil; w.orderOut(nil)
        NSApp.stop(nil)
        NSApp.postEvent(NSEvent.otherEvent(with: .applicationDefined, location: .zero, modifierFlags: [], timestamp: 0, windowNumber: 0, context: nil, subtype: 0, data1: 0, data2: 0)!, atStart: true)   // run returns at the next event
    }
}

/// The settings' hosting view. A click on a window that is not the key window acts on what it lands on, as AppKit's own controls let it: a SwiftUI button takes such
/// a click, a gesture (the sliding choice) only when its hosting view does
final class SettingsHost<Content: View>: NSHostingView<Content> {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

/// The window's content view: its origin at the top left, so the settings keep their place while the window's bottom edge moves
private final class TopDown: NSView {
    override var isFlipped: Bool { true }
}

enum LauncherWindowLayout {
    /// The window's frame at a height. The top edge never moves: the row that was clicked to open Advanced settings stays under the pointer, so a second click closes it
    /// again. A window with too little screen below it stops at the screen's bottom edge, and the settings scroll
    static func fitted(_ frame: NSRect, height: CGFloat, visible: NSRect) -> NSRect {
        let top = min(frame.maxY, visible.maxY)
        let h = min(height, top - visible.minY)
        return NSRect(x: frame.minX, y: top - h, width: frame.width, height: h)
    }
}

// 1: Play (the window's settings are in the environment), 0: the player quit, 2: no window (no launcher.json, or "Show this window at launch" off): launcher.c applies the saved choices
@_cdecl("dm_mac_launcher")
public func dmMacLauncher() -> Int32 {
    guard let url = Bundle.main.resourceURL?.appendingPathComponent("dm/launcher.json"), let data = try? Data(contentsOf: url) else { return 2 }
    guard let file = try? JSONDecoder().decode(SettingsFile.self, from: data), !file.settings.isEmpty else { fputs("[RED] launcher: dm/launcher.json cannot be read; starting the game\n", stderr); return 2 }
    if (UserDefaults.standard.object(forKey: "window.show") as? Bool) == false && !NSEvent.modifierFlags.contains(.option) {
        fputs("launcher: settings window off (hold Option at launch to show it)\n", stderr)
        return SaveCheck.withoutWindow() ? 2 : 0
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
