// The launcher's Players section (Launcher.swift): the connected controllers as the game will take them, a Joy-Con pair's hold, and the player
// order. DmGamepad.m assigns the players when the game starts, from the same saved keys.
import GameController
import SwiftUI

/// The players: the connected controllers (GameController) in the player order chosen here, each pair of Joy-Con held upright (one player) or
/// sideways (one player each half), and which of them the game takes. DmGamepad.m reads the same keys when the game starts: a controller's
/// key is its name and which of that name it is in GameController's order (pad_key), players.order the order of keys, players.joyconSideways.<pair
/// number> how a pair is held (players.joyconSideways the default; DM_JOYCON_SIDEWAYS in the environment wins), players.max how many players the game took when it last ran.
struct ControllersSection: View {
    @State private var pads: [GCController] = GCController.controllers()
    @State private var revision = 0   // bumped by a change: the order and the holds are read again
    private let defaults = UserDefaults.standard

    private static func name(_ c: GCController) -> String { c.vendorName ?? c.productCategory }
    private static func isJoyConPair(_ c: GCController) -> Bool {
        let n = (c.productCategory + (c.vendorName ?? "")).lowercased()
        return c.extendedGamepad != nil && (n.contains("joy-con") || n.contains("joycon"))
    }
    private func key(_ c: GCController) -> String {
        var k = 0
        for o in pads { if Self.name(o) == Self.name(c) { k += 1 }; if o === c { break } }
        return "\(Self.name(c))#\(k)"
    }
    private func pairNumber(_ c: GCController) -> Int {
        var n = 0
        for o in pads { if Self.isJoyConPair(o) { n += 1 }; if o === c { break } }
        return n
    }
    private func sideways(_ c: GCController) -> Bool {
        let k = "players.joyconSideways.\(pairNumber(c))"
        return defaults.object(forKey: k) != nil ? defaults.bool(forKey: k) : defaults.bool(forKey: "players.joyconSideways")
    }
    private var ordered: [GCController] {
        _ = revision
        let order = defaults.stringArray(forKey: "players.order") ?? []
        return pads.enumerated().sorted {
            let a = order.firstIndex(of: key($0.element)) ?? Int.max, b = order.firstIndex(of: key($1.element)) ?? Int.max
            return a != b ? a < b : $0.offset < $1.offset
        }.map(\.element)
    }
    private var accepted: Int { _ = revision; return defaults.integer(forKey: "players.max") }   // 0: not known yet
    private func move(_ c: GCController, by step: Int) {
        var list = ordered
        guard let i = list.firstIndex(where: { $0 === c }), list.indices.contains(i + step) else { return }
        list.swapAt(i, i + step)
        defaults.set(list.map(key), forKey: "players.order")
        revision += 1
    }
    private func setSideways(_ c: GCController, _ on: Bool) {
        defaults.set(on, forKey: "players.joyconSideways.\(pairNumber(c))")
        revision += 1
    }
    /// One row per player: a controller, or a half of a pair held sideways
    private struct Player: Identifiable { let id: String; let pad: GCController; let label: String; let first: Bool }
    private var players: [Player] {
        ordered.flatMap { c -> [Player] in
            let n = Self.name(c)
            guard Self.isJoyConPair(c) else { return [Player(id: key(c), pad: c, label: n, first: true)] }
            let pair = "Joy-Con pair \(pairNumber(c))"
            return sideways(c) ? [Player(id: key(c) + "L", pad: c, label: pair + " · left", first: true),
                                  Player(id: key(c) + "R", pad: c, label: pair + " · right", first: false)]
                               : [Player(id: key(c), pad: c, label: pair, first: true)]
        }
    }

    var body: some View {
        let list = players
        Section {
            if pads.isEmpty { Text("No controller connected").foregroundStyle(.secondary) }
            ForEach(Array(list.enumerated()), id: \.element.id) { i, p in
                let used = accepted == 0 || i < accepted
                HStack(spacing: 12) {
                    Text(used ? "P\(i + 1)" : "—").font(.body.monospacedDigit().weight(.semibold)).frame(width: 34, alignment: .leading)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(p.label)
                        if !used { Text("Not used: the game takes \(accepted) player\(accepted == 1 ? "" : "s")").font(.caption).foregroundStyle(.secondary) }
                    }
                    Spacer()
                    if p.first && Self.isJoyConPair(p.pad) {
                        Picker("", selection: Binding(get: { sideways(p.pad) }, set: { setSideways(p.pad, $0) })) {
                            Text("Upright").tag(false)
                            Text("Sideways").tag(true)
                        }
                        .pickerStyle(.segmented).frame(width: 190).labelsHidden()
                    }
                    if p.first && pads.count > 1 {
                        Button { move(p.pad, by: -1) } label: { Image(systemName: "chevron.up") }
                            .buttonStyle(.borderless).disabled(ordered.first === p.pad)
                        Button { move(p.pad, by: 1) } label: { Image(systemName: "chevron.down") }
                            .buttonStyle(.borderless).disabled(ordered.last === p.pad)
                    }
                }
                .foregroundStyle(used ? .primary : .secondary)
            }
        } header: {
            Text(accepted > 0 ? "Players (this game takes up to \(accepted))" : "Players")
        } footer: {
            Text(pads.isEmpty ? "Connect a controller in Settings › Bluetooth; the touch controls are used until then."
                              : "Each controller is a player of its own, in this order (↑ ↓). A Joy-Con pair held upright is one player, sideways one player each Joy-Con, with its L / ZL or R / ZR as L / R (iPadOS does not pass on SL / SR). Which Joy-Con make a pair is up to iPadOS: connect them again in the order you want them paired.")
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in pads = GCController.controllers() }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in pads = GCController.controllers() }
    }
}
