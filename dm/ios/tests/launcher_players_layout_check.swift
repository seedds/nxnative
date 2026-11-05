// Exercise the real SwiftUI/AppKit bridge with the real native player panel.
// A window is allocated but never shown; no game, GPU or physical input is used.
import AppKit
import SwiftUI

@_silgen_name("test_players_view")
private func nativePlayersView() -> UnsafeMutableRawPointer
@_silgen_name("test_players_change")
private func nativePlayersChange()

private var panel: NSView!
@_cdecl("dm_launcher_players_view")
func playersViewStub(_ enabled: Int32) -> UnsafeMutableRawPointer? {
    guard enabled == 1 else { return nil }
    if panel == nil {
        if let path = ProcessInfo.processInfo.environment["NXNATIVE_TEST_HOST_LIBRARY"] {
            guard let library = dlopen(path, RTLD_NOW | RTLD_LOCAL),
                  let symbol = dlsym(library, "dm_mac_players_view") else { fatalError("cannot load test host library") }
            let makeView = unsafeBitCast(symbol, to: (@convention(c) () -> UnsafeMutableRawPointer?).self)
            guard let pointer = makeView() else { fatalError("host library returned no panel") }
            panel = Unmanaged<NSView>.fromOpaque(pointer).takeUnretainedValue()
        } else {
            panel = Unmanaged<NSView>.fromOpaque(nativePlayersView()).takeUnretainedValue()
        }
    }
    return Unmanaged.passUnretained(panel).toOpaque()
}
@_cdecl("dm_launcher_players_finish")
func playersFinishStub(_ play: Int32) { fatalError("test never starts the game") }

@main struct Check {
    static func main() {
        NSApplication.shared.setActivationPolicy(.prohibited)
        let suite = "nxnative.players-layout-test.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let model = LauncherModel([])
        model.values["DM_PLAYER_SETUP"] = "1"
        let view = NSHostingView(rootView: LauncherView(m: model, done: { _ in }, fit: { _ in }).defaultAppStorage(defaults))
        view.sizingOptions = []
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 660),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.contentView = view
        // A detached NSHostingView misses AppKit's ancestor layout in the crash report.
        for width: CGFloat in [520, 560] {
            view.frame = NSRect(x: 0, y: 0, width: width, height: 660)
            for _ in 0..<20 {
                view.layoutSubtreeIfNeeded()
                RunLoop.current.run(until: Date().addingTimeInterval(0.01))
            }
            assert(!window.isVisible)
            assert(panel.superview != nil, "the real representable must embed the native panel")
            assert(panel.frame.height > 0)
        }
        if ProcessInfo.processInfo.environment["NXNATIVE_TEST_HOST_LIBRARY"] == nil {
            for expanded in [true, false] {
                let previousHeight = panel.frame.height
                nativePlayersChange()
                for _ in 0..<20 {
                    view.layoutSubtreeIfNeeded()
                    RunLoop.current.run(until: Date().addingTimeInterval(0.01))
                }
                assert(panel.frame.height > 0 && !window.isVisible)
                assert(expanded ? panel.frame.height > previousHeight : panel.frame.height < previousHeight,
                       "the hosted chooser must expand and collapse without re-entering SwiftUI layout")
            }
        }
        print("launcher players: hosted layout passed without visible windows")
    }
}
