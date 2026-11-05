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
}

struct ControllerChoice: Equatable {
    var player: Int? = -1
    var style: GameControllerStyle = .auto
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
        guard let channel else { return true }
        let rows: [[String: Any]] = controllers.map { device in
            let choice = choices[device.id] ?? ControllerChoice()
            return ["id": device.id, "player": choice.player ?? 0, "style": choice.style.token]
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
        var next = choices.filter { ids.contains($0.key) }
        for device in devices where next[device.id] == nil {
            next[device.id] = ControllerChoice()
        }
        choices = next
        controllers = devices
    }

    func choosePlayer(_ player: Int?, for id: LauncherController.ID) {
        guard var choice = choices[id] else { return }
        if let player, player != -1 && !(1...8).contains(player) { return }
        // Move the displaced device to this device's old slot (or Unassigned).
        // There is always at most one device in each player slot, even in the draft.
        if let player, player > 0, let other = choices.first(where: { $0.key != id && $0.value.player == player }) {
            choices[other.key]?.player = choice.player
        }
        choice.player = player
        choices[id] = choice
    }

    func resolvedPlayer(for id: String) -> Int? {
        if ProcessInfo.processInfo.environment["DM_PAD_MERGE"] == "1",
           choices.values.allSatisfy({ $0.player == -1 && $0.style == .auto }) { return 1 }
        var used = Set(choices.values.compactMap { choice -> Int? in
            guard let p = choice.player, p > 0 else { return nil }
            return p
        })
        for device in controllers {
            let choice = choices[device.id] ?? ControllerChoice()
            let player: Int?
            if choice.player == -1 {
                player = (1...8).first { !used.contains($0) }
                if let player { used.insert(player) }
            } else { player = choice.player }
            if device.id == id { return player }
        }
        return nil
    }

    func automaticStyle(for id: String) -> String {
        resolvedPlayer(for: id) == 1 && operationMode() == "0" ? "Handheld" : "Pro Controller"
    }

    func chooseStyle(_ style: GameControllerStyle, for id: LauncherController.ID) {
        guard choices[id] != nil else { return }
        choices[id]?.style = style
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
                        set: { model.choosePlayer($0 == 0 ? nil : $0, for: controller.id) })) {
                        Text(model.resolvedPlayer(for: controller.id).map { "Player \($0) • Automatic" } ?? "Automatic").tag(-1)
                        Text("Unassigned").tag(0)
                        ForEach(1...8, id: \.self) { Text("Player \($0)").tag($0) }
                    }
                    Picker("Game sees", selection: Binding(
                        get: { model.choices[controller.id]?.style ?? .auto },
                        set: { model.chooseStyle($0, for: controller.id) })) {
                        ForEach(GameControllerStyle.allCases) { Text($0 == .auto ? model.automaticStyle(for: controller.id) + " • Automatic" : $0.rawValue).tag($0) }
                    }
                } header: {
                    Text("Controller \(index + 1)")
                }
            }
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
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }.padding(20)
        }
        .frame(width: 600, height: 600)
        .onExitCommand { dismiss() }
    }
}
