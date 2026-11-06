// Exercise session drafts with same-named devices, without needing paired hardware.
import AppKit
import SwiftUI
import Darwin

@main struct ControllerSettingsCheck {
    static func main() {
        if CommandLine.arguments.count == 3 && CommandLine.arguments[1] == "--channel" {
            let channel = ControllerLaunchChannel(fd: Int32(CommandLine.arguments[2])!)
            let model = ControllerSettingsModel(channel: channel)
            let end = Date().addingTimeInterval(5)
            while !model.ready && model.error == nil && Date() < end { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
            _ = ProcessInfo.processInfo.environment
            setenv("NX_CONTROLLER_TEST_START", "from-window", 1)
            guard model.ready && model.start() else { exit(1) }
            print("controller channel: OK")
            exit(0)
        }
        var failed = 0
        func expect(_ c: Bool, _ what: String) { print((c ? "PASS " : "FAIL ") + what); if !c { failed += 1 } }
        let a = LauncherController(id: "device-a", name: "DualSense")
        let b = LauncherController(id: "device-b", name: "DualSense")
        let c = LauncherController(id: "device-c", name: "DualSense")
        let model = ControllerSettingsModel()
        let beforeEnv = ProcessInfo.processInfo.environment
        let beforePrefs = UserDefaults.standard.dictionaryRepresentation() as NSDictionary
        model.update([a, b])
        expect(model.choices[a.id]?.player == -1 && model.choices[b.id]?.player == -1 && model.resolvedPlayer(for: a.id) == 1 && model.resolvedPlayer(for: b.id) == 2, "same-named controllers get independent player drafts")
        model.chooseStyle(.left, for: a.id)
        expect(model.choices[a.id]?.style == .left && model.choices[b.id]?.style == .auto, "one controller's style does not affect another")
        model.update([b, a])
        expect(model.choices[a.id] == ControllerChoice(player: -1, style: .left), "discovery reorder preserves identity and choices")
        model.choosePlayer(2, for: a.id)
        expect(model.choices[a.id]?.player == 2 && model.resolvedPlayer(for: b.id) == 1, "moving to an occupied player swaps the drafts")
        model.choosePlayer(nil, for: b.id)
        model.choosePlayer(2, for: b.id)
        expect(model.choices[b.id]?.player == 2 && model.choices[a.id]?.player == nil, "moving an unassigned device displaces the occupied device to unassigned")
        model.choosePlayer(9, for: b.id)
        expect(model.choices[b.id]?.player == 2, "invalid player slots are ignored")
        model.update([b])
        expect(model.choices[a.id] == nil, "disconnect removes the draft")
        model.update([b, c])
        expect(model.choices[c.id] == ControllerChoice(), "a same-named replacement does not inherit the disconnected device's style")
        expect(ControllerSettingsModel().choices.isEmpty, "a new session starts without saved choices")
        expect(ProcessInfo.processInfo.environment == beforeEnv, "drafts do not change the host environment")
        expect(beforePrefs.isEqual(to: UserDefaults.standard.dictionaryRepresentation()), "drafts do not write preferences")

        if CommandLine.arguments.count > 1 {
            let dir = URL(fileURLWithPath: CommandLine.arguments[1])
            try! FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            NSApplication.shared.setActivationPolicy(.prohibited)
            model.chooseStyle(.right, for: c.id)
            capture(ControllerSettingsSheet(model: model), to: dir.appendingPathComponent("connected.png"))
            capture(LauncherView(m: LauncherModel([]), controllers: model, done: { _ in }, fit: { _ in }), to: dir.appendingPathComponent("launch-window.png"))
            model.update([])
            capture(ControllerSettingsSheet(model: model), to: dir.appendingPathComponent("empty.png"))
        }
        print("controller settings: \(failed) failed")
        exit(failed == 0 ? 0 : 1)
    }

    static func capture<V: View>(_ root: V, to url: URL) {
        let view = NSHostingView(rootView: root.background(Color(nsColor: .windowBackgroundColor)).environment(\.colorScheme, .light))
        view.frame = NSRect(x: 0, y: 0, width: 600, height: 600)
        let window = NSWindow(contentRect: view.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = view
        window.setFrameOrigin(NSPoint(x: -10000, y: -10000))
        window.orderBack(nil)
        let end = Date().addingTimeInterval(0.5)
        while Date() < end { view.layoutSubtreeIfNeeded(); RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds)!
        view.cacheDisplay(in: view.bounds, to: bitmap)
        try! bitmap.representation(using: .png, properties: [:])!.write(to: url)
        window.orderOut(nil)
    }
}
