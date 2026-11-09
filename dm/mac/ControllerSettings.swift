// Session-only choices in the generated game's prelaunch window. Device identifiers
// come from the engine process, which retains ownership of input, motion and haptics.
import Foundation
import Darwin
import SwiftUI

enum GameControllerStyle: String, CaseIterable, Identifiable {
    case auto = "Automatic"
    case pro = "Pro Controller"
    case pair = "Joy-Con pair"
    case left = "Joy-Con (L)"
    case right = "Joy-Con (R)"
    var id: Self { self }
    var token: String {
        switch self { case .auto: return "auto"; case .pro: return "pro"; case .pair: return "pair"; case .left: return "left"; case .right: return "right" }
    }
}

struct LauncherController: Identifiable, Decodable {
    let id: String
    let name: String
    let automaticPlayer: Int?
    let reservedPlayers: [Int]?
    let physicalSide: String?
    init(id: String, name: String, automaticPlayer: Int? = nil, reservedPlayers: [Int]? = nil, physicalSide: String? = nil) {
        self.id=id; self.name=name; self.automaticPlayer=automaticPlayer
        self.reservedPlayers=reservedPlayers; self.physicalSide=physicalSide
    }
}

enum ControllerPairRole: String, CaseIterable, Identifiable {
    case both, left, right
    var id: Self { self }
    var title: String { self == .both ? "One controller" : self == .left ? "Left controller" : "Right controller" }
}
struct ControllerChoice: Equatable {
    var player: Int = -1
    var style: GameControllerStyle = .auto
    var role: ControllerPairRole = .both
}

final class ControllerSettingsModel: ObservableObject {
    @Published private(set) var controllers: [LauncherController] = []
    @Published private(set) var choices: [LauncherController.ID: ControllerChoice] = [:]
    @Published private(set) var ready: Bool
    @Published private(set) var error: String?
    @Published private(set) var identification: (id: String, message: String)?
    @Published private(set) var signaled: Set<String> = []
    var operationMode: () -> String? = { ProcessInfo.processInfo.environment["DM_OPMODE"] }
    private let channel: ControllerLaunchChannel?

    init(channel: ControllerLaunchChannel? = nil) {
        self.channel = channel
        ready = channel == nil
        channel?.snapshot = { [weak self] devices in
            self?.update(devices)
            self?.ready = true
        }
        channel?.failed = { [weak self] message in self?.error = message }
        channel?.identified = { [weak self] id, ok in
            if ok { self?.signaled.insert(id) }
            guard self?.identification?.id == id else { return }
            self?.identification = ok ? nil : (id, "This controller couldn’t be signaled.")
        }
        channel?.listen()
    }

    func start() -> Bool {
        guard pairingIssue == nil else { return false }
        guard let channel else { return true }
        let rows: [[String: Any]] = controllers.map { device in
            let choice = choices[device.id] ?? ControllerChoice()
            return ["id": device.id, "player": choice.player, "style": choice.style.token, "role": choice.role.rawValue]
        }
        return channel.start(environment: ProcessInfo.processInfo.environment, choices: rows, explicit: choices.values.contains { $0.player != -1 || $0.style != .auto })
    }

    func cancel() { channel?.cancel() }

    func identify(_ id: String) {
        let sent = channel?.identify(id) == true
        identification = (id, sent ? "Signaling controller…" : "This controller couldn’t be signaled.")
    }

    /// Discovery updates preserve choices for the same live object, including two devices
    /// with identical names. Disconnecting drops the draft rather than guessing an identity.
    func update(_ devices: [LauncherController]) {
        let ids = Set(devices.map(\.id))
        if let identification, !ids.contains(identification.id) { self.identification = nil }
        choices = devices.reduce(into: [:]) { $0[$1.id] = choices[$1.id] ?? ControllerChoice() }
        controllers = devices
    }

    func choosePlayer(_ player: Int, for id: LauncherController.ID) {
        guard var choice = choices[id] else { return }
        guard (-1...8).contains(player) else { return }
        if player>0 {
            let occupants=controllers.filter { $0.id != id && resolvedPlayer(for:$0.id)==player }
            if occupants.count==1, let other=occupants.first, var peer=choices[other.id] {
                let ownSide=controllers.first { $0.id==id }?.physicalSide.flatMap(ControllerPairRole.init(rawValue:))
                let peerSide=other.physicalSide.flatMap(ControllerPairRole.init(rawValue:))
                let side=peerSide ?? ownSide.map { $0 == .left ? .right : .left } ?? (peer.role == .both ? .left : peer.role)
                let opposite: ControllerPairRole = side == .left ? .right : .left
                if ownSide == nil || ownSide == opposite {
                    peer.player=player; peer.style = .pair; peer.role=side
                    choice.player=player; choice.style = .pair; choice.role=opposite
                    choices[other.id]=peer; choices[id]=choice; return
                }
            }
        }
        if player > 0 {
            let others = choices.filter { $0.key != id && $0.value.player == player }
            for (key, target) in others {
                let shares = choice.style == .pair && target.style == .pair && choice.role != .both && target.role != .both && choice.role != target.role
                if !shares {
                    let swaps = others.count == 1 && choice.role == .both && target.role == .both
                    choices[key]?.player = swaps ? choice.player : -1
                    if !swaps { choices[key]?.role = .both }
                }
            }
        }
        choice.player = player
        if player <= 0 { choice.role = .both }
        choices[id] = choice
    }

    func resolvedPlayer(for id: String) -> Int? {
        if ProcessInfo.processInfo.environment["DM_PAD_MERGE"] == "1",
           choices.values.allSatisfy({ $0.player == -1 && $0.style == .auto }) { return 1 }
        var used = Set(choices.values.map(\.player).filter { $0 > 0 })
        used.formUnion(controllers.flatMap { $0.reservedPlayers ?? [] })
        for device in controllers {
            let choice = choices[device.id] ?? ControllerChoice()
            let player: Int?
            if choice.player == -1 {
                let preferred=device.automaticPlayer
                player = preferred.flatMap { used.contains($0) ? nil : $0 } ?? (1...8).first { !used.contains($0) }
                if let player { used.insert(player) }
            } else { player = choice.player > 0 ? choice.player : nil }
            if device.id == id { return player }
        }
        return nil
    }

    // The Automatic row stands for its resolved player. Offer that player
    // once, while retaining the full fixed-player list after a manual choice.
    func playerSlots(for id: String) -> [Int] {
        let automatic = choices[id]?.player == -1 ? resolvedPlayer(for: id) : nil
        return (1...8).filter { $0 != automatic }
    }

    func automaticPlayerLabel(for id: String) -> String {
        guard choices[id]?.player == -1, let player = resolvedPlayer(for: id) else { return "Automatic" }
        return "Player \(player) • Automatic"
    }

    var pairingIssue: String? {
        var roles: [Int: Set<ControllerPairRole>] = [:]
        for c in choices.values where c.role != .both {
            guard c.player>0 else { return "Choose a player for each paired controller." }
            roles[c.player,default: []].insert(c.role)
        }
        return roles.values.contains(where: { $0 != Set([.left,.right]) }) ? "Assign a left and a right controller to the same player to complete the pair." : nil
    }
    func chooseRole(_ role: ControllerPairRole, for id: String) {
        guard var choice=choices[id] else { return }
        if role != .both { choice.player=resolvedPlayer(for:id) ?? 1 }
        for key in choices.keys where key != id && choices[key]?.player == choice.player && choice.player > 0 {
            if role == .both || choices[key]?.style != .pair || choices[key]?.role == .both || (choices[key]?.role == role && choice.role == .both) {
                choices[key]?.player = -1; choices[key]?.role = .both
            } else if choices[key]?.role == role { choices[key]?.role = choice.role }
        }
        choice.role=role; choices[id]=choice
    }
    func automaticStyle(for id: String) -> String {
        resolvedPlayer(for: id) == 1 && operationMode() == "0" ? "Handheld" : "Pro Controller"
    }

    func chooseStyle(_ style: GameControllerStyle, for id: LauncherController.ID) {
        guard choices[id] != nil else { return }
        if style != .pair { chooseRole(.both,for:id) }
        choices[id]?.style = style
        if style == .pair, let side=controllers.first(where: { $0.id==id })?.physicalSide, let role=ControllerPairRole(rawValue:side) { chooseRole(role,for:id) }
    }
}

/// Device choices are kept in the configuration sheet.
struct ControllerSettingsFields: View {
    @ObservedObject var model: ControllerSettingsModel

    var body: some View {
        if let error = model.error {
            Section { Text(error).foregroundStyle(.red) }
        } else if !model.ready {
            Section { ProgressView("Finding controllers…") }
        } else if model.controllers.isEmpty {
            Section {
                Text("No controllers connected").foregroundStyle(.secondary)
                Text("Connect a controller in macOS Bluetooth settings or with a USB cable.")
                    .font(.callout).foregroundStyle(.secondary)
            }
        } else {
            ForEach(Array(model.controllers.enumerated()), id: \.element.id) { index, controller in
                Section {
                    LabeledContent("Connected controller") {
                        HStack {
                            Text(controller.name)
                            Button(model.signaled.contains(controller.id) ? "Identify • signal sent" : "Identify") { model.identify(controller.id) }
                        }
                    }
                    if let identification = model.identification, identification.id == controller.id {
                        Text(identification.message).font(.callout).foregroundStyle(.secondary)
                    }
                    Picker("Player", selection: Binding(
                        get: { model.choices[controller.id]?.player ?? 0 },
                        set: { model.choosePlayer($0, for: controller.id) })) {
                        Text(model.automaticPlayerLabel(for: controller.id)).tag(-1)
                        Text("Unassigned").tag(0)
                        ForEach(model.playerSlots(for: controller.id), id: \.self) { Text("Player \($0)").tag($0) }
                    }
                    Picker("Game sees", selection: Binding(
                        get: { model.choices[controller.id]?.style ?? .auto },
                        set: { model.chooseStyle($0, for: controller.id) })) {
                        ForEach(GameControllerStyle.allCases) { Text($0 == .auto ? model.automaticStyle(for: controller.id) + " • Automatic" : $0.rawValue).tag($0) }
                    }
                    if model.choices[controller.id]?.style == .pair {
                        Picker("Pair input", selection: Binding(get: { model.choices[controller.id]?.role ?? .both }, set: { model.chooseRole($0,for:controller.id) })) {
                            ForEach(ControllerPairRole.allCases.filter { controller.physicalSide == nil || $0.rawValue == controller.physicalSide }) { Text($0.title).tag($0) }
                        }
                        if model.choices[controller.id]?.role == .both {
                            Text("Both Joy-Cons share this controller's motion. Separate hand movements need two controllers.").font(.callout).foregroundStyle(.secondary)
                        }
                    }
                } header: {
                    Text("Controller \(index + 1)")
                }
            }
            if let issue=model.pairingIssue { Section { Text(issue).foregroundStyle(.red) } }
        }
    }
}

struct ControllerSettingsSheet: View {
    @ObservedObject var model: ControllerSettingsModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Controllers").font(.title2.weight(.semibold))
                Spacer()
            }.padding(20)
            Form {
                ControllerSettingsFields(model: model)
            }.formStyle(.grouped)
            HStack {
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction).disabled(model.pairingIssue != nil)
            }.padding(20)
        }
        .frame(width: 600, height: 600)
        .onExitCommand { dismiss() }
    }
}
