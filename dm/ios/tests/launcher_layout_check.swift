// The Mac settings window (dm/mac/Launcher.swift), run by test_launcher_storage.py: the height its content reports for the window, measured on the real view without
// creating or showing a window, the frame the window takes for a height, and what is worked out from a value (how much of the header the bar holds, a folder's path).
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
    var basic: CGFloat = 0
    for width: CGFloat in [520, 560] {
        // each measurement in its own pool: a hosting view left autoreleased stays alive and lays out its content again on every later change of window.advanced
        let closed = autoreleasepool { measure(false, width: width) }, open = autoreleasepool { measure(true, width: width) }
        expect(closed > 0 && open > closed, "Advanced settings adds to the reported height")
        basic = closed
    }
    // Show this window at launch, the window's last line, says how to see the window again only while it is switched off: one more line, which the window makes room for
    defaults.set(false, forKey: "window.show")
    let hinted = autoreleasepool { measure(false, width: 560) }
    defaults.removeObject(forKey: "window.show")
    expect(hinted > basic + 4 && hinted < basic + 30, "switched off, Show this window at launch adds its one line to the reported height (\(hinted) after \(basic))")
    // The header scrolls with the settings and goes into the bar a part at a time (LauncherView.scrolled, TitleBar): its icon, then its name, then its Play, each
    // over the 14 pt of scroll after it has gone under the bar, in eighths (the icon's in the hundreds, the name's in the tens, Play's in the units), and none
    // before the header is measured
    let at = { LauncherView.scrolled($0, header: 230, name: 150) }
    expect(at(0) == 0 && at(96) == 0 && at(103) == 400 && at(110) == 800 && at(150) == 800 && at(157) == 840 && at(164) == 880 && at(216) == 880 && at(223) == 884 && at(230) == 888 && at(900) == 888,
           "the bar takes the header's icon, then its name, then its Play, each with the scroll that follows its going under the bar")
    expect(LauncherView.scrolled(500, header: 0, name: 0) == 0 && LauncherView.scrolled(500, header: 230, name: 0) == 0, "and nothing while the header is not measured")
    // Every row's symbol is a symbol of this system (a name it lacks draws nothing) or one drawn here, in rows of one length
    let missing = (Array(Symbols.rows.values) + ["gearshape"]).filter { Symbols.drawn[$0] == nil && NSImage(systemSymbolName: $0, accessibilityDescription: nil) == nil }
    expect(missing.isEmpty, "every row's symbol is one this system has, or is drawn (missing: \(missing))")
    let squares = Symbols.drawn.values.compactMap { drawn -> [String]? in if case .squares(let rows) = drawn { return rows }; return nil }
    expect(!squares.isEmpty && squares.allSatisfy { rows in !rows.isEmpty && rows.allSatisfy { $0.count == rows[0].count && $0.allSatisfy { "#.".contains($0) } } } && Symbols.drawn.keys.allSatisfy { Symbols.rows.values.contains($0) },
           "a symbol drawn from squares is a rectangle of them, and every drawn symbol has a row")
    // The cloud folder's path is shortened by whole folders (CloudSavesRow.fitting), under iCloud Drive and outside it
    expect(CloudSavesRow.fitting(CloudSavesRow.drive + "/Games/Switch Saves/Zelda") == ["iCloud Drive › Games › Switch Saves › Zelda", "… › Games › Switch Saves › Zelda", "… › Switch Saves › Zelda", "… › Zelda"],
           "a cloud folder's path leaves out leading folders, whole ones")
    expect(CloudSavesRow.fitting("/Volumes/SSD/saves") == ["/Volumes/SSD/saves", "…/SSD/saves", "…/saves"] && CloudSavesRow.fitting("/saves") == ["/saves"] && CloudSavesRow.fitting(CloudSavesRow.drive) == ["iCloud Drive"],
           "so does a path outside iCloud Drive; a single folder is shown whole")
    // The window keeps a margin to the screen's top and bottom edges, by the screen's height (LauncherWindowLayout.margin): 56 pt on this one
    let visible = NSRect(x: -1920, y: 40, width: 1920, height: 900)
    func screen(_ height: CGFloat) -> NSRect { NSRect(x: 0, y: 0, width: 1800, height: height) }
    expect(LauncherWindowLayout.margin(visible) == 56 && LauncherWindowLayout.margin(screen(1046)) == 64 && LauncherWindowLayout.margin(screen(1410)) == 64
           && LauncherWindowLayout.margin(screen(730)) == 25 && LauncherWindowLayout.margin(screen(600)) == 20, "the margin grows with the screen, from 20 to 64 pt")
    let initial = NSRect(x: -1500, y: 540, width: 560, height: 400)   // its top edge at the top of the screen
    let expanded = LauncherWindowLayout.fitted(initial, height: 700, visible: visible)
    let capped = LauncherWindowLayout.fitted(initial, height: 1600, visible: visible)
    let collapsed = LauncherWindowLayout.fitted(expanded, height: 400, visible: visible)
    expect(expanded.maxY == initial.maxY && expanded.height == 700, "expansion preserves the top edge")
    expect(capped == NSRect(x: -1500, y: 152, width: 560, height: 788), "tall content is no taller than the screen less a margin above and below")
    expect(collapsed == initial, "collapse restores the content height and top edge")
    let centred = NSRect(x: -1500, y: 300, width: 560, height: 400)   // its top edge 240 pt below the top of the screen
    expect(LauncherWindowLayout.fitted(centred, height: 900, visible: visible) == NSRect(x: -1500, y: 96, width: 560, height: 604), "a window lower on the screen keeps its top edge and stops a margin above the screen's bottom")
    let low = NSRect(x: -1500, y: 40, width: 560, height: 100)   // dragged to the screen's bottom edge
    expect(LauncherWindowLayout.fitted(low, height: 900, visible: visible) == NSRect(x: -1500, y: -180, width: 560, height: 320), "a window at the screen's bottom edge is not closed to a strip")
    let small = screen(730), top = NSRect(x: 100, y: 330, width: 560, height: 400)
    expect(LauncherWindowLayout.fitted(top, height: 672, visible: small).height == 672 && LauncherWindowLayout.fitted(top, height: 1490, visible: small).height == 680,
           "a small laptop's screen shows the basic settings whole, and no less with Advanced settings open")
    return failed
}
