// The Mac settings window (dm/mac/Launcher.swift), run by test_launcher_storage.py: measure the real grouped Form of each tab without creating or showing a window.
import AppKit
import SwiftUI

func checkLauncherLayout(_ model: LauncherModel, defaults: UserDefaults) -> Int {
    NSApplication.shared.setActivationPolicy(.prohibited)
    var failed = 0
    func expect(_ c: Bool, _ what: String) { print((c ? "PASS " : "FAIL ") + what); if !c { failed += 1 } }
    let tabs = model.sections + [LauncherView.cloudTab]
    expect(tabs.first == "Game" && tabs.last == LauncherView.cloudTab && Set(tabs).count == tabs.count, "one tab per section, Cloud saves last (\(tabs.joined(separator: ", ")))")
    func measure(_ tab: String, width: CGFloat) -> CGFloat? {
        defaults.set(tab, forKey: "window.tab")
        var extra: CGFloat?
        let view = NSHostingView(rootView: LauncherView(m: model, done: { _ in }, overflow: { extra = $0 }).defaultAppStorage(defaults))
        view.sizingOptions = []
        view.frame = NSRect(x: 0, y: 0, width: width, height: LauncherView.height)
        // SwiftUI reports the Form's scroll geometry on a later pass, an estimate first and the measured rows a few ms after it, later still on a slow runner (issue #445):
        // run the loop until the report has held for 0.2 s; after 5 s the check sees whatever was reported last
        let limit = Date().addingTimeInterval(5)
        var held = extra, since = Date()
        while Date() < limit {
            view.layoutSubtreeIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
            if extra != held { held = extra; since = Date() } else if extra != nil, Date().timeIntervalSince(since) >= 0.2 { break }
        }
        return extra
    }
    for width: CGFloat in [520, 560] {
        for tab in tabs {
            let extra = autoreleasepool { measure(tab, width: width) }
            expect(extra != nil, "the \(tab) tab reports its scroll content size (width \(width))")
            if let e = extra { expect(e <= 0, "the \(tab) tab fits the window without scrolling (width \(width), \(Int(-e)) pt to spare)") }
        }
    }
    defaults.set("Removed tab", forKey: "window.tab")
    expect(autoreleasepool { measure("Removed tab", width: 560) } != nil, "a remembered tab the game no longer has shows the first tab")
    defaults.removeObject(forKey: "window.tab")
    return failed
}
