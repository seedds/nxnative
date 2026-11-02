// Mac app launcher, Input tab: the controls profile (launcher.c controls_profile) as a keyboard map, the controller buttons and the controller sticks. A profile is a text file of
// KEY=VALUE lines in ~/Library/Application Support/nxnative/controls/<name>.txt, values as the host takes them (dm/host/input_map.h): DM_KEYMAP, DM_PAD_MAP, DM_PAD_ABXY, DM_PAD_STICK_DZ, DM_PAD_STICK_SENS.
// Default.txt is shared by every game and is the template of a new profile; the profile a game uses is controls.profile in its app's preferences (none: Default).
// Every change is written to the file at once; a key or button left as the host's default is not written, so a profile holds only what the player changed.
import AppKit
import SwiftUI

enum Controls {
    // input_map.h im_names and key_map_default; dm/tools/tests/test_nsp_mac_app.py checks that they match
    static let names = ["A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down",
                        "LUp", "LDown", "LLeft", "LRight", "RUp", "RDown", "RLeft", "RRight", "Shake"]
    static let labels = ["A", "B", "X", "Y", "Press left stick", "Press right stick", "L", "R", "ZL", "ZR", "+", "−", "D-pad left", "D-pad up", "D-pad right", "D-pad down",
                         "Left stick up", "Left stick down", "Left stick left", "Left stick right", "Right stick up", "Right stick down", "Right stick left", "Right stick right", "Shake"]
    static let buttons = 16
    static let defaultKeys: [Int] = [38, 40, 32, 34, 3, 5, 12, 14, 6, 8, 36, 48, 123, 126, 124, 125, 13, 1, 0, 2, -1, -1, -1, -1, 49]
    static let folder = URL(fileURLWithPath: NSHomeDirectory() + "/Library/Application Support/nxnative/controls")
    static let dzMax = 0.5, sensRange = 0.25...3.0
    static let faceModes = [("auto", "Auto"), ("label", "By label"), ("position", "By position")]   // DM_PAD_ABXY values, input_map.h pad_abxy_parse

    static func keyName(_ code: Int) -> String {
        if code < 0 { return "None" }
        let table: [Int: String] = [
            0: "A", 1: "S", 2: "D", 3: "F", 4: "H", 5: "G", 6: "Z", 7: "X", 8: "C", 9: "V", 11: "B", 12: "Q", 13: "W", 14: "E", 15: "R", 16: "Y", 17: "T",
            18: "1", 19: "2", 20: "3", 21: "4", 22: "6", 23: "5", 24: "=", 25: "9", 26: "7", 27: "-", 28: "8", 29: "0", 30: "]", 31: "O", 32: "U", 33: "[", 34: "I", 35: "P",
            36: "Return", 37: "L", 38: "J", 39: "'", 40: "K", 41: ";", 42: "\\", 43: ",", 44: "/", 45: "N", 46: "M", 47: ".", 48: "Tab", 49: "Space", 50: "`", 51: "Delete",
            53: "Esc", 65: "Keypad .", 67: "Keypad *", 69: "Keypad +", 71: "Clear", 75: "Keypad /", 76: "Enter", 78: "Keypad -", 81: "Keypad =",
            82: "Keypad 0", 83: "Keypad 1", 84: "Keypad 2", 85: "Keypad 3", 86: "Keypad 4", 87: "Keypad 5", 88: "Keypad 6", 89: "Keypad 7", 91: "Keypad 8", 92: "Keypad 9",
            96: "F5", 97: "F6", 98: "F7", 99: "F3", 100: "F8", 101: "F9", 103: "F11", 109: "F10", 111: "F12", 114: "Help", 115: "Home", 116: "Page Up", 117: "Forward Delete",
            118: "F4", 119: "End", 120: "F2", 121: "Page Down", 122: "F1", 123: "←", 124: "→", 125: "↓", 126: "↑"]
        return table[code] ?? "Key \(code)"
    }

    static func validName(_ s: String) -> Bool {
        !s.isEmpty && s.count <= 40 && !s.hasPrefix(".") && s.allSatisfy { $0.isLetter || $0.isNumber || " -_".contains($0) }
    }
}

final class ControlsModel: ObservableObject {
    @Published var profiles: [String] = []
    @Published var keys = Controls.defaultKeys      // the key code each function is on, -1 none
    @Published var pad = Array(0..<Controls.buttons)   // the button each controller button sends, -1 none
    @Published var dz = 0.0
    @Published var sens = 1.0
    @Published var abxy = "auto"
    @Published var profile: String { didSet { UserDefaults.standard.set(profile, forKey: "controls.profile"); load() } }
    /// The controls the launch environment or an env file set: they win over the profile, so the tab says so
    let fixed: [String] = {
        let env = ProcessInfo.processInfo.environment
        return ["DM_KEYMAP", "DM_PAD_MAP", "DM_PAD_ABXY", "DM_PAD_STICK_DZ", "DM_PAD_STICK_SENS"].filter { env[$0] != nil }
    }()

    init() {
        let p = UserDefaults.standard.string(forKey: "controls.profile") ?? "Default"
        profile = Controls.validName(p) ? p : "Default"
        refresh(); load()
    }

    private func url(_ name: String) -> URL { Controls.folder.appendingPathComponent(name + ".txt") }

    func refresh() {
        let files = (try? FileManager.default.contentsOfDirectory(atPath: Controls.folder.path)) ?? []
        let names = files.filter { $0.hasSuffix(".txt") }.map { String($0.dropLast(4)) }.filter { Controls.validName($0) && $0 != "Default" }.sorted()
        profiles = ["Default"] + names
        if !profiles.contains(profile) { profiles.append(profile); profiles.sort { $0 == "Default" || ($1 != "Default" && $0 < $1) } }
    }

    var isDefault: Bool { keys == Controls.defaultKeys && pad == Array(0..<Controls.buttons) && dz == 0 && sens == 1 && abxy == "auto" }

    func load() {
        keys = Controls.defaultKeys; pad = Array(0..<Controls.buttons); dz = 0; sens = 1; abxy = "auto"
        guard let text = try? String(contentsOf: url(profile), encoding: .utf8) else { return }
        for line in text.split(whereSeparator: \.isNewline) {
            guard let eq = line.firstIndex(of: "="), !line.hasPrefix("#") else { continue }
            let k = String(line[..<eq]), v = String(line[line.index(after: eq)...])
            switch k {
            case "DM_KEYMAP": entries(v) { f, val in
                guard let code = val.lowercased() == "none" ? -1 : Int(val), code < 128, f < Controls.names.count else { return }
                self.assign(f, code) }
            case "DM_PAD_MAP": entries(v) { f, val in
                guard f < Controls.buttons else { return }
                if val.lowercased() == "none" { self.pad[f] = -1 } else if let t = Self.index(val), t < Controls.buttons { self.pad[f] = t } }
            case "DM_PAD_STICK_DZ": if let x = Double(v), (0...Controls.dzMax).contains(x) { dz = x }
            case "DM_PAD_STICK_SENS": if let x = Double(v), Controls.sensRange.contains(x) { sens = x }
            case "DM_PAD_ABXY": if let m = Controls.faceModes.first(where: { $0.0.caseInsensitiveCompare(v) == .orderedSame }) { abxy = m.0 }
            default: break
            }
        }
    }

    private static func index(_ name: String) -> Int? { Controls.names.firstIndex { $0.caseInsensitiveCompare(name) == .orderedSame } }
    private func entries(_ spec: String, _ one: (Int, String) -> Void) {
        for e in spec.split(separator: ",") {
            let p = e.split(separator: "=", maxSplits: 1).map(String.init)
            if p.count == 2, let f = Self.index(p[0]) { one(f, p[1]) }
        }
    }

    /// As the host: a function goes to one key, and the function that held the key before is left on no key
    func assign(_ f: Int, _ code: Int) {
        if code >= 0 { for i in keys.indices where keys[i] == code { keys[i] = -1 } }
        keys[f] = code
    }

    func text() -> String {
        var lines: [String] = []
        let km = keys.indices.filter { keys[$0] != Controls.defaultKeys[$0] }.map { "\(Controls.names[$0])=\(keys[$0] < 0 ? "none" : String(keys[$0]))" }
        let pm = pad.indices.filter { pad[$0] != $0 }.map { "\(Controls.names[$0])=\(pad[$0] < 0 ? "none" : Controls.names[pad[$0]])" }
        if !km.isEmpty { lines.append("DM_KEYMAP=" + km.joined(separator: ",")) }
        if !pm.isEmpty { lines.append("DM_PAD_MAP=" + pm.joined(separator: ",")) }
        if abxy != "auto" { lines.append("DM_PAD_ABXY=" + abxy) }
        if dz != 0 { lines.append(String(format: "DM_PAD_STICK_DZ=%.2f", dz)) }
        if sens != 1 { lines.append(String(format: "DM_PAD_STICK_SENS=%.2f", sens)) }
        return lines.map { $0 + "\n" }.joined()
    }

    func save() { save(as: profile) }
    private func save(as name: String) {
        do {
            try FileManager.default.createDirectory(at: Controls.folder, withIntermediateDirectories: true)
            try text().write(to: url(name), atomically: true, encoding: .utf8)
        } catch { fputs("[RED] launcher: cannot write the controls profile \(name): \(error.localizedDescription)\n", stderr) }
    }

    /// A new profile starts as a copy of the one shown (Default the first time), and this game uses it
    func create(_ name: String) {
        guard Controls.validName(name), !profiles.contains(name) else { return }
        save(as: name); profiles.append(name); refresh(); profile = name
    }
    func delete() {
        guard profile != "Default" else { return }
        try? FileManager.default.removeItem(at: url(profile))
        profile = "Default"; refresh()
    }
    func reset() { keys = Controls.defaultKeys; pad = Array(0..<Controls.buttons); dz = 0; sens = 1; abxy = "auto"; save() }
}

struct ControlsTab: View {
    @ObservedObject var c: ControlsModel
    @State private var recording: Int?
    @State private var monitor: Any?
    @State private var naming = false
    @State private var newName = ""

    var body: some View {
        Section {
            Picker("Profile", selection: $c.profile) { ForEach(c.profiles, id: \.self) { Text($0).tag($0) } }
            HStack {
                Button("New Profile…") { newName = ""; naming = true }
                Button("Delete Profile") { c.delete() }.disabled(c.profile == "Default")
                Spacer()
                Button("Reset Profile") { c.reset() }.disabled(c.isDefault)
            }
        } footer: {
            Text(c.profile == "Default" ? "Default is shared by every game and is the starting point of a new profile." : "This game uses \(c.profile). Other games keep their own choice.")
        }
        .alert("New Profile", isPresented: $naming) {
            TextField("Name", text: $newName)
            Button("Create") { c.create(newName.trimmingCharacters(in: .whitespaces)) }.disabled(!Controls.validName(newName.trimmingCharacters(in: .whitespaces)) || c.profiles.contains(newName))
            Button("Cancel", role: .cancel) {}
        } message: { Text("It starts as a copy of \(c.profile).") }

        if !c.fixed.isEmpty {
            Section { Text("Set by env.txt or the launch environment, which win over this profile: \(c.fixed.joined(separator: ", ")).").foregroundStyle(.orange) }
        }

        Section {
            LabeledContent("Dead zone") {
                HStack { Slider(value: $c.dz, in: 0...Controls.dzMax, step: 0.01) { Text("Dead zone") }.labelsHidden(); Text(String(format: "%.2f", c.dz)).monospacedDigit().frame(width: 36, alignment: .trailing) }
            }
            LabeledContent("Sensitivity") {
                HStack { Slider(value: $c.sens, in: Controls.sensRange, step: 0.05) { Text("Sensitivity") }.labelsHidden(); Text(String(format: "%.2f", c.sens)).monospacedDigit().frame(width: 36, alignment: .trailing) }
            }
        } header: { Text("Controller sticks") } footer: {
            Text("Dead zone: how far a stick moves before the game sees it. Sensitivity: above 1, a smaller tilt reaches full speed.")
        }
        .onChange(of: c.dz) { c.save() }
        .onChange(of: c.sens) { c.save() }

        Section {
            Picker("Face buttons", selection: Binding(get: { c.abxy }, set: { c.abxy = $0; c.save() })) {
                ForEach(Controls.faceModes, id: \.0) { Text($0.1).tag($0.0) }
            }
            ForEach(0..<Controls.buttons, id: \.self) { b in
                Picker(Controls.labels[b], selection: Binding(get: { c.pad[b] }, set: { c.pad[b] = $0; c.save() })) {
                    ForEach(0..<Controls.buttons, id: \.self) { Text(Controls.labels[$0]).tag($0) }
                    Divider()
                    Text("None").tag(-1)
                }
            }
        } header: { Text("Controller buttons") } footer: {
            Text("Face buttons: Auto goes by position (right is A), or by label on a Nintendo controller. By label: the button marked A sends A. By position: the bottom button sends B, as on a Switch. Then what each button sends to the game.")
        }

        Section {
            ForEach(Controls.names.indices, id: \.self) { f in
                LabeledContent(Controls.labels[f]) {
                    HStack(spacing: 6) {
                        Button(recording == f ? "Press a key…" : Controls.keyName(c.keys[f])) { record(f) }
                            .frame(minWidth: 110)
                        Button { c.assign(f, -1); c.save() } label: { Image(systemName: "xmark.circle") }
                            .buttonStyle(.borderless).help("No key").disabled(c.keys[f] < 0)
                    }
                }
            }
        } header: { Text("Keyboard") } footer: {
            Text("Click a key, then press the new one; Esc cancels. A key holds one function: the function it had is left on no key. Shift, Control, Option and Command cannot be used.")
        }
        .onDisappear { stop() }
    }

    private func record(_ f: Int) {
        stop(); recording = f
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { e in
            guard let f = recording else { return e }
            if e.keyCode != 53 && e.keyCode < 128 { c.assign(f, Int(e.keyCode)); c.save() }
            stop()
            return nil
        }
    }
    private func stop() {
        if let m = monitor { NSEvent.removeMonitor(m) }
        monitor = nil; recording = nil
    }
}
