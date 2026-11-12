// The Mac settings window (dm/mac/Launcher.swift), run by test_launcher_storage.py: the height its content reports for the window, measured on the real view without
// creating or showing a window, and the frame the window takes for a height.
import AppKit
import SwiftUI

func checkLauncherLayout(_ model: LauncherModel, defaults: UserDefaults) -> Int {
    NSApplication.shared.setActivationPolicy(.prohibited)
    var failed = 0
    func expect(_ c: Bool, _ what: String) { print((c ? "PASS " : "FAIL ") + what); if !c { failed += 1 } }
    func measure(_ advanced: Bool, width: CGFloat) -> CGFloat {
        defaults.set(advanced, forKey: "window.advanced")
        var wanted: CGFloat?, reports = 0
        let view = NSHostingView(rootView: LauncherView(m: model, done: { _ in }, fit: { wanted = $0; reports += 1 }).defaultAppStorage(defaults))
        view.sizingOptions = []
        // The content reports its height on a pass after it is laid out, later on a slow runner (issue #445): run the loop until the report meets the step's
        // expectation and has held for 0.2 s; after 5 s the check sees whatever was reported last
        func settle(_ want: (CGFloat) -> Bool) {
            let limit = Date().addingTimeInterval(5)
            var held = wanted, since = Date()
            while Date() < limit {
                view.layoutSubtreeIfNeeded()
                RunLoop.current.run(until: Date().addingTimeInterval(0.01))
                if wanted != held { held = wanted; since = Date() } else if let h = wanted, want(h), Date().timeIntervalSince(since) >= 0.2 { return }
            }
        }
        view.frame = NSRect(x: 0, y: 0, width: width, height: 660)
        settle { _ in true }
        guard let height = wanted else { expect(false, "the content reports its height"); return 0 }
        expect(height > 300 && height < 4000, "the content reports a height a window can take (advanced=\(advanced), width=\(width), height=\(height))")
        // The height is the content's own: the window taking it, or a window the screen keeps shorter, changes nothing (a height that followed the window's would
        // be chased for ever, as the grouped Form's scroll geometry was through its safe-area bars)
        let before = reports
        view.frame.size.height = height
        settle { _ in true }
        view.frame.size.height = 400
        settle { _ in true }
        expect(wanted == height && reports == before, "the reported height does not follow the window's (\(reports - before) more reports, \(wanted ?? 0) after \(height))")
        defaults.set(!advanced, forKey: "window.advanced")
        settle { advanced ? $0 < height : $0 > height }
        expect(advanced ? (wanted ?? height) < height : (wanted ?? 0) > height, "the same view reports expansion and collapse after its saved state changes")
        return height
    }
    for width: CGFloat in [520, 560] {
        // each measurement in its own pool: a hosting view left autoreleased stays alive and lays out its content again on every later change of window.advanced
        let closed = autoreleasepool { measure(false, width: width) }, open = autoreleasepool { measure(true, width: width) }
        expect(closed > 0 && open > closed, "Advanced settings adds to the reported height")
    }
    let visible = NSRect(x: -1920, y: 40, width: 1920, height: 900)
    let initial = NSRect(x: -1500, y: 540, width: 560, height: 400)   // its top edge at the top of the screen
    let expanded = LauncherWindowLayout.fitted(initial, height: 700, visible: visible)
    let capped = LauncherWindowLayout.fitted(initial, height: 1600, visible: visible)
    let collapsed = LauncherWindowLayout.fitted(expanded, height: 400, visible: visible)
    expect(expanded.maxY == initial.maxY && expanded.height == 700, "expansion preserves the top edge")
    expect(capped == NSRect(x: -1500, y: 40, width: 560, height: 900), "tall content stops at the visible screen frame")
    expect(collapsed == initial, "collapse restores the content height and top edge")
    let centred = NSRect(x: -1500, y: 300, width: 560, height: 400)   // its top edge 240 pt below the top of the screen
    expect(LauncherWindowLayout.fitted(centred, height: 900, visible: visible) == NSRect(x: -1500, y: 40, width: 560, height: 660), "a window lower on the screen keeps its top edge and stops at the screen's bottom")
    return failed
}
