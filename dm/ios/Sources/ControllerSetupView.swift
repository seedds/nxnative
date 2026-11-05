import GameController
import SwiftUI
import UIKit

/// Shared launch and runtime form. The engine owns the device IDs and validates each submitted draft.
struct ControllerSetupView: View {
    private enum Style: String, CaseIterable, Identifiable {
        case auto, pro, pair, left, right

        var id: Self { self }
        var title: String {
            switch self {
            case .auto: String(localized: "Automatic")
            case .pro: String(localized: "Pro Controller")
            case .pair: String(localized: "Joy-Con pair")
            case .left: String(localized: "Joy-Con (L)")
            case .right: String(localized: "Joy-Con (R)")
            }
        }
    }

    private struct Device: Identifiable, Equatable {
        let id: String
        let name: String
        var player: Int
        var style: Style

        var choice: [String: Any] { ["id": id, "player": player, "style": style.rawValue] }
    }

    private struct Route: Equatable {
        let player: Int
        let ready: Bool
        let automaticStyle: String
        let recommendation: Style?
    }

    private struct Readiness: Equatable {
        let minimum: Int
        let maximum: Int
        let styles: [Style]
        let players: [Int]
        let ready: Int
        let touch: Bool
        let routes: [String: Route]

        init(_ status: [String: Any]) {
            minimum = status["minimum"] as? Int ?? 0
            maximum = status["maximum"] as? Int ?? 8
            styles = (status["styles"] as? [String] ?? []).compactMap(Style.init(rawValue:))
            players = status["players"] as? [Int] ?? []
            ready = status["ready"] as? Int ?? 0
            touch = status["touch"] as? Bool ?? false
            routes = (status["routes"] as? [String: [String: Any]] ?? [:]).mapValues {
                Route(player: $0["player"] as? Int ?? 0, ready: $0["ready"] as? Bool ?? false,
                      automaticStyle: $0["automaticStyle"] as? String ?? "pro",
                      recommendation: ($0["recommendation"] as? String).flatMap(Style.init(rawValue:)))
            }
        }

        var canApply: Bool { (minimum <= ready) && (ready <= maximum) }

        var playerCount: String {
            if minimum > maximum { return String(localized: "The game requested conflicting player counts.") }
            if minimum == maximum {
                return minimum == 1 ? String(localized: "1 player required") : String(localized: "\(minimum) players required")
            }
            if minimum == 0 {
                return maximum == 8 ? String(localized: "No player count requested") : String(localized: "Up to \(maximum) players")
            }
            return String(localized: "\(minimum)–\(maximum) players required")
        }
    }

    var runtimeCompletion: ((Bool) -> Void)? = nil
    private var inGame: Bool { runtimeCompletion != nil }
    @Environment(\.dismiss) private var dismiss
    @State private var devices: [Device] = []
    @State private var error: String?
    @State private var readiness: Readiness?
    @State private var signaled: Set<String> = []
    @State private var recommendedRequirements: [Style]?
    @State private var recommendationSeen: Set<String> = []

    var body: some View {
        NavigationStack {
            Form {
                if inGame, let readiness { requirements(readiness) }
                if devices.isEmpty {
                    ContentUnavailableView("No controllers connected", systemImage: "gamecontroller",
                                           description: Text("Connect a controller in Bluetooth settings or plug it in."))
                }
                ForEach(Array(devices.enumerated()), id: \.element.id) { index, device in
                    Section {
                        Picker("Player", selection: Binding(get: { player(for: device.id) },
                                                             set: { assign($0, to: device.id) })) {
                            Text(automaticPlayerLabel(device.id)).tag(-1)
                            Text("Unassigned").tag(0)
                            ForEach(1...8, id: \.self) { Text("Player \($0)").tag($0) }
                        }
                        Picker("Game sees", selection: Binding(get: { style(for: device.id) },
                                                                set: { setStyle($0, for: device.id) })) {
                            ForEach(Style.allCases) { Text(styleLabel($0, for: device.id)).tag($0) }
                        }
                        Button(signaled.contains(device.id) ? "Identify • signal sent" : "Identify") {
                            if dm_ios_controller_identify(device.id) {
                                signaled.insert(device.id)
                            } else {
                                signaled.remove(device.id)
                                error = String(localized: "This controller couldn’t be signaled.")
                            }
                        }
                        .accessibilityLabel(Text("Identify \(device.name), controller \(index + 1)"))
                        if let route = readiness?.routes[device.id] {
                            if inGame, let readiness { assignmentStatus(device, route, readiness) }
                        } else if device.player != 0 {
                            Text("No player slot available").font(.footnote).foregroundStyle(.secondary)
                        }
                    } header: {
                        Text(controllerHeading(device, index: index))
                    }
                }
                if !inGame, !devices.isEmpty {
                    Section {
                        Text("Changes apply to this session.").font(.footnote).foregroundStyle(.secondary)
                    }
                }
            }
            .safeAreaInset(edge: .bottom) {
                if inGame, let readiness { readinessFooter(readiness) }
            }
            .navigationTitle("Controllers")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { finish(false) } }
                ToolbarItem(placement: .confirmationAction) {
                    if !inGame {
                        Button("Done") { save(devices.map(\.choice)) }.disabled(devices.isEmpty)
                    }
                }
            }
        }
        .onAppear(perform: refresh)
        .onChange(of: devices) { _, _ in updateReadiness() }
        .task {
            guard inGame else { return }
            while !Task.isCancelled {
                updateReadiness()
                try? await Task.sleep(for: .milliseconds(250))
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect).receive(on: RunLoop.main)) { _ in refresh() }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect).receive(on: RunLoop.main)) { _ in refresh() }
        .alert("Controllers", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK") { error = nil }
        } message: {
            Text(error ?? "")
        }
    }

    private func requirements(_ status: Readiness) -> some View {
        Section("Game requirements") {
            Text(status.playerCount).font(.headline)
            if status.styles.isEmpty {
                Text("No external controller types accepted")
            } else {
                LabeledContent("Accepted types", value: status.styles.map(\.title).joined(separator: ", "))
            }
            if status.players.isEmpty {
                Text("No external player slots available")
            } else if status.players.count < 8 {
                LabeledContent("Player slots", value: status.players.map(String.init).joined(separator: ", "))
            }
        }
    }

    @ViewBuilder private func assignmentStatus(_ device: Device, _ route: Route, _ status: Readiness) -> some View {
        let slotAccepted = status.players.contains(route.player) || (route.player == 1 && device.style == .auto)
        if !route.ready {
            Text(slotAccepted ? "Change “Game sees” to an accepted type." : "Choose a player slot listed above.")
                .font(.footnote).foregroundStyle(.secondary)
        }
    }

    private func readinessFooter(_ status: Readiness) -> some View {
        HStack(spacing: 16) {
            VStack(alignment: .leading, spacing: 3) {
                if status.minimum > status.maximum {
                    Text("Setup unavailable").font(.headline)
                    Text("Cancel this setup and try again.").font(.footnote)
                } else if status.ready < status.minimum {
                    Text("\(status.ready) of \(status.minimum) players ready").font(.headline)
                    Text("Assign the required players to continue.").font(.footnote)
                } else {
                    Text(status.ready == 1 ? "1 player ready" : "\(status.ready) players ready").font(.headline)
                    if status.ready > status.maximum {
                        Text("Unassign \(status.ready - status.maximum) to continue.").font(.footnote)
                    } else {
                        Text("Changes apply to this session.").font(.footnote)
                    }
                }
                if status.touch {
                    Text("Touch controls count as Player 1.").font(.footnote)
                }
            }
            .accessibilityElement(children: .combine)
            Spacer(minLength: 0)
            Button("Apply") { save(devices.map(\.choice)) }
                .buttonStyle(.borderedProminent)
                .disabled(!status.canApply)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 20)
        .padding(.vertical, 8)
        .background(.regularMaterial)
    }

    private func updateReadiness() {
        let status = dm_ios_controller_status(devices.map(\.choice), inGame)
        readiness = status["error"] == nil ? Readiness(status) : nil
        guard inGame, let readiness else { return }
        if recommendedRequirements != readiness.styles {
            recommendedRequirements = readiness.styles
            recommendationSeen.removeAll()
        }
        for index in devices.indices {
            let id = devices[index].id
            guard recommendationSeen.insert(id).inserted,
                  let route = readiness.routes[id], !route.ready,
                  let recommended = route.recommendation else { continue }
            // Accepted choices and later user edits stay intact. Only an
            // incompatible initial draft receives a game-specific suggestion.
            let effectiveType = devices[index].style == .auto
                ? Style(rawValue: route.automaticStyle) : devices[index].style
            if effectiveType.map(readiness.styles.contains) != true {
                devices[index].style = recommended
            }
        }
    }

    private func refresh() {
        let previous = Dictionary(uniqueKeysWithValues: devices.map { ($0.id, $0) })
        let snapshot = dm_ios_controller_snapshot().compactMap { row -> Device? in
            guard let id = row["id"] as? String, let name = row["name"] as? String,
                  let player = row["player"] as? Int, let token = row["style"] as? String,
                  let style = Style(rawValue: token) else { return nil }
            return Device(id: id, name: name, player: player, style: style)
        }
        devices = snapshot.map { previous[$0.id] ?? $0 }
        updateReadiness()
    }

    private func controllerHeading(_ device: Device, index: Int) -> String {
        let title = "\(device.name) · \(index + 1)"
        if inGame, device.player != 0, readiness?.routes[device.id]?.ready != true {
            return title + " • " + String(localized: "Not ready")
        }
        return title
    }

    private func automaticPlayerLabel(_ id: String) -> String {
        guard let route = readiness?.routes[id] else { return String(localized: "Automatic") }
        return String(localized: "Player \(route.player) • Automatic")
    }

    private func styleLabel(_ style: Style, for id: String) -> String {
        if style == .auto {
            guard let route = readiness?.routes[id] else { return style.title }
            let title = route.automaticStyle == "handheld" ? String(localized: "Handheld") : Style(rawValue: route.automaticStyle)?.title ?? style.title
            return "\(title) • " + String(localized: "Automatic")
        }
        if inGame, readiness?.styles.contains(style) == true {
            return style.title + " • " + String(localized: "Recommended")
        }
        return style.title
    }

    private func player(for id: String) -> Int { devices.first { $0.id == id }?.player ?? -1 }
    private func style(for id: String) -> Style { devices.first { $0.id == id }?.style ?? .auto }

    private func setStyle(_ style: Style, for id: String) {
        guard let index = devices.firstIndex(where: { $0.id == id }) else { return }
        devices[index].style = style
    }

    private func assign(_ player: Int, to id: String) {
        guard let index = devices.firstIndex(where: { $0.id == id }) else { return }
        if player > 0, let other = devices.firstIndex(where: { $0.player == player && $0.id != id }) {
            devices[other].player = devices[index].player
        }
        devices[index].player = player
    }

    private func save(_ choices: [[String: Any]]) {
        let message = inGame ? dm_ios_controller_apply(choices) : dm_ios_controller_configure(choices)
        if let message {
            error = message
            refresh()
        } else {
            finish(true)
        }
    }

    private func finish(_ applied: Bool) {
        if let runtimeCompletion { runtimeCompletion(applied) } else { dismiss() }
    }
}

@objc(DmControllerSetupHost)
final class DmControllerSetupHost: NSObject {
    @MainActor @objc(makeControllerWithCompletion:)
    static func makeController(completion: @escaping (Bool) -> Void) -> UIViewController {
        DmHostingController(rootView: ControllerSetupView(runtimeCompletion: completion))
    }
}
