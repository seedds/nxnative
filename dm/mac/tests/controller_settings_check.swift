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
        expect(model.automaticPlayerLabel(for: a.id) == "Player 1 • Automatic" && !model.playerSlots(for: a.id).contains(1), "Automatic player 1 has one menu entry")
        expect(model.automaticPlayerLabel(for: b.id) == "Player 2 • Automatic" && !model.playerSlots(for: b.id).contains(2), "Automatic player 2 has one menu entry")
        model.chooseStyle(.left, for: a.id)
        expect(model.choices[a.id]?.style == .left && model.choices[b.id]?.style == .auto, "one controller's style does not affect another")
        model.update([b, a])
        expect(model.choices[a.id] == ControllerChoice(player: -1, style: .left), "discovery reorder preserves identity and choices")
        model.choosePlayer(2, for: a.id)
        expect(model.automaticPlayerLabel(for: a.id) == "Automatic" && model.playerSlots(for: a.id) == Array(1...8), "manual assignment keeps every fixed player and an unambiguous return to Automatic")
        expect(model.choices[a.id]?.player == 2 && model.resolvedPlayer(for: b.id) == 1, "fixing the already resolved slot does not group another player")
        model.choosePlayer(0, for: b.id)
        expect(model.choices[b.id]?.player == 0 && model.resolvedPlayer(for: b.id) == nil, "Unassigned uses the protocol's zero without resolving a player")
        model.choosePlayer(2, for: b.id)
        expect(model.choices[b.id]?.player == 2 && model.choices[a.id]?.player == 2 && model.pairingIssue == nil, "an unassigned device joins the occupied player as a pair")
        model.choosePlayer(9, for: b.id)
        expect(model.choices[b.id]?.player == 2, "invalid player slots are ignored")
        model.update([b])
        expect(model.choices[a.id] == nil, "disconnect removes the draft")
        model.update([b, c])
        expect(model.choices[c.id] == ControllerChoice(), "a same-named replacement does not inherit the disconnected device's style")
        expect(ControllerSettingsModel().choices.isEmpty, "a new session starts without saved choices")
        expect(ProcessInfo.processInfo.environment == beforeEnv, "drafts do not change the host environment")
        expect(beforePrefs.isEqual(to: UserDefaults.standard.dictionaryRepresentation()), "drafts do not write preferences")

        let automaticPair = ControllerSettingsModel(); automaticPair.update([a,b])
        automaticPair.choosePlayer(1,for:b.id)
        expect(automaticPair.choices[a.id]?.player == 1 && automaticPair.choices[b.id]?.player == 1 && automaticPair.choices[a.id]?.role == .left && automaticPair.choices[b.id]?.role == .right && automaticPair.pairingIssue == nil, "joining an Automatic occupant forms a complete pair")

        let sameSide = ControllerSettingsModel()
        sameSide.update([LauncherController(id:"left-a",name:"Joy-Con L",physicalSide:"left"),LauncherController(id:"left-b",name:"Joy-Con L",physicalSide:"left")])
        sameSide.choosePlayer(1,for:"left-b")
        expect(sameSide.choices["left-a"]?.style != .pair && sameSide.choices["left-b"]?.style != .pair, "two physical left halves never form an invented left/right pair")

        let pair = ControllerSettingsModel(); pair.update([a,b])
        pair.chooseStyle(.pair,for:a.id); pair.chooseStyle(.pair,for:b.id)
        pair.chooseRole(.left,for:a.id)
        expect(pair.pairingIssue != nil && !pair.start(), "a half pair cannot start the game")
        pair.chooseRole(.right,for:b.id); pair.choosePlayer(1,for:b.id)
        expect(pair.choices[a.id]?.player == 1 && pair.choices[b.id]?.player == 1 && pair.pairingIssue == nil, "complementary roles share one player")
        pair.chooseRole(.right,for:a.id)
        expect(pair.choices[b.id]?.role == .left && pair.pairingIssue == nil, "swapping sides preserves the complete pair")
        pair.chooseRole(.left,for:a.id)
        pair.chooseRole(.both,for:a.id)
        expect(pair.choices[b.id]?.player == -1 && pair.choices[b.id]?.role == .both && pair.pairingIssue == nil, "single-pad pair returns the second controller to automatic")

        pair.update([a,b,c]); pair.chooseRole(.left,for:a.id)
        pair.chooseRole(.right,for:b.id); pair.choosePlayer(1,for:b.id)
        pair.chooseStyle(.pair,for:c.id); pair.chooseRole(.left,for:c.id); pair.choosePlayer(1,for:c.id)
        expect(pair.choices[a.id]?.player == -1 && pair.choices[a.id]?.role == .both && pair.choices[b.id]?.player == 1 && pair.choices[c.id]?.player == 1 && pair.pairingIssue == nil, "a third source replaces only the conflicting side")

        let split = ControllerSettingsModel()
        let leftHalf = LauncherController(id: "half-l", name: "Joy-Con (L)", automaticPlayer: 2, reservedPlayers: [1], physicalSide: "left")
        let rightHalf = LauncherController(id: "half-r", name: "Joy-Con (R)", automaticPlayer: 3, reservedPlayers: [1], physicalSide: "right")
        split.update([leftHalf,rightHalf])
        expect(split.resolvedPlayer(for:leftHalf.id)==2 && split.resolvedPlayer(for:rightHalf.id)==3, "launch preview preserves the backend's keyboard reservation")
        split.chooseStyle(.pair,for:leftHalf.id); split.chooseStyle(.pair,for:rightHalf.id)
        split.choosePlayer(2,for:rightHalf.id)
        expect(split.choices[leftHalf.id]?.role == .left && split.choices[rightHalf.id]?.role == .right && split.pairingIssue == nil, "physical halves use their existing side in a shared player")

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
