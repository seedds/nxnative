// Launcher settings, validation and persistence. Shared by the iOS UI, the Mac app's settings window (dm/mac/Launcher.swift) and the native storage regression test.
import Foundation
import Combine

struct Setting: Decodable, Identifiable {
    let env, kind, label, note, def: String
    let section: String?   // heading on the settings screen; absent in older bundles
    let needs: String?     // "<VAR>=<value>": a sub-setting, drawn only while that setting has that value
    let level: String?     // "basic": shown first (the Mac window's Basic settings); any other or none: Advanced settings. The iPhone keeps one list
    let base: Base?        // render scale: the 3D views' size at 1.0, docked and handheld, for the size in pixels (the Mac window)
    struct Base: Decodable { let docked, handheld: [Int] }
    let options: [[String]]?   // choice: [value, label]
    let min, max, step: Double?   // range
    let on, off: String?   // toggle: the values the variable takes (1 / 0 when not given)
    var id: String { env }
    var sectionTitle: String { section ?? "Game" }
    var basic: Bool { level == "basic" }
    var onValue: String { on ?? "1" }
    var offValue: String { off ?? "0" }
    func isDefault(_ v: String) -> Bool {   // a range compares as numbers (a slider writes 1.00 for 1.0)
        kind == "range" ? abs((Double(v) ?? .nan) - (Double(def) ?? .nan)) < 1e-6 : v == def
    }
    func accepts(_ v: String) -> Bool {   // a saved choice the game still offers
        switch kind {
        case "choice": return options?.contains { $0[0] == v } ?? false
        case "range":
            guard !v.contains(where: { $0 == "x" || $0 == "X" }), let n = Double(v), n.isFinite, let lo = min, let hi = max else { return false }   // no hex: launcher.c's check agrees
            return n >= lo && n <= hi
        case "toggle": return v == onValue || v == offValue
        default: return true
        }
    }
    enum CodingKeys: String, CodingKey { case env, kind, label, note, def = "default", section, needs, level, base, options, min, max, step, on, off }
}
struct SettingsFile: Decodable {
    let settings: [Setting]

    static func bundledSettings() -> [Setting] {
        // Multi-program apps load the running program's settings from its dm/p<N>.
        let dir = getenv("DM_APP_DM").map { URL(fileURLWithPath: String(cString: $0)) } ?? Bundle.main.resourceURL?.appendingPathComponent("dm")
        guard let url = dir?.appendingPathComponent("launcher.json"), let data = try? Data(contentsOf: url) else { return [] }
        guard let file = try? JSONDecoder().decode(Self.self, from: data) else {
            fputs("[RED] launcher: dm/launcher.json cannot be read\n", stderr)
            return []
        }
        return file.settings
    }
}

struct LauncherUser: Identifiable {
    let id, nickname, label: String
    let index: Int32

    static func read() -> [LauncherUser] {
        let users = (0..<dm_users_count()).map {
            (name: String(cString: dm_users_nickname_at($0)), id: String(cString: dm_users_id_at($0)))
        }
        return users.enumerated().map { index, user in
            let peers = users.filter { $0.name == user.name && $0.id != user.id }
            var label = user.name
            if !peers.isEmpty {
                let suffix = user.id.suffix(8), short = "\(user.name) · \(suffix)"
                let ambiguous = peers.contains { $0.id.suffix(8) == suffix } || users.contains { $0.name == short }
                label = ambiguous ? "\(user.name) · \(user.id)" : short
            }
            return LauncherUser(id: user.id, nickname: user.name, label: label, index: Int32(index))
        }
    }
}

final class LauncherModel: ObservableObject {
    let settings: [Setting]
    let fixed: Set<String>      // set by an env file or by the launch environment: shown, not changed
    let fromLaunch: Set<String>   // of those, the ones the launch environment set (a front end's launch overrides, Xcode; DM_ENV_LAUNCH_KEYS from launcher.c or main.m)
    let ignoreSaved: Bool   // DM_SAVED_CHOICES=0: start from the defaults, and leave the saved choices as they are
    @Published private(set) var users = LauncherUser.read()
    let userLocked = dm_users_selection_locked() != 0
    @Published var selectedUser = dm_users_selected_index()
    var pickedUser: LauncherUser? { users.first { $0.index == selectedUser } }
    var personalSaveIsLocal: Bool { pickedUser.map { $0.id != DM_LEGACY_USER_ID } ?? false }
    var canEditUsers: Bool { dm_users_can_edit() != 0 }
    var canCreateUser: Bool { canEditUsers && users.count < Int(DM_MAX_LOCAL_USERS) }

    static func nicknameProblem(_ name: String) -> String? {
        if name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return "Enter a nickname." }
        if name.utf8.count > 32 { return "Use a nickname of 32 UTF-8 bytes or fewer." }
        if name.rangeOfCharacter(from: .controlCharacters) != nil { return "Remove line breaks and control characters from the nickname." }
        return nil
    }

    func editUser(renameID: String?, nickname: String) -> String? {
        if let problem = Self.nicknameProblem(nickname) { return problem }
        if let id = renameID, !users.contains(where: { $0.id == id }) { return "Choose a user from the list to rename." }
        if let problem = dm_users_edit(renameID, nickname) { return String(cString: problem) }
        users = LauncherUser.read()
        selectedUser = dm_users_selected_index()
        return nil
    }
    @Published var values: [String: String] = [:]

    private let defaults: UserDefaults

    init(_ settings: [Setting], defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.settings = settings
        let env = ProcessInfo.processInfo.environment
        fromLaunch = Set((env["DM_ENV_LAUNCH_KEYS"] ?? "").split(separator: " ").map(String.init))
        fixed = fromLaunch.union((env["DM_ENV_FILE_KEYS"] ?? "").split(separator: " ").map(String.init))
        ignoreSaved = env["DM_SAVED_CHOICES"] == "0"
        if ignoreSaved { fputs("launcher: saved choices ignored (DM_SAVED_CHOICES=0)\n", stderr) }
        for s in settings {
            if fixed.contains(s.env), let v = env[s.env] { values[s.env] = v; continue }
            let saved = ignoreSaved ? nil : defaults.object(forKey: "launcher." + s.env) as? String   // a string, as launcher.c reads it: not string(forKey:), which turns a number into one
            values[s.env] = saved.flatMap { s.accepts($0) ? $0 : nil } ?? s.def   // a saved choice the game no longer offers: its default
        }
    }

    var sections: [String] {
        settings.reduce(into: []) { if !$0.contains($1.sectionTitle) { $0.append($1.sectionTitle) } }
    }
    // A row with "needs" is a sub-setting of the row it names: drawn only while that setting has that value, so a
    // setting that cannot do anything is not offered (an upscaler's detail with the upscaler off, rumble strength
    // with rumble off). A row naming a setting this game does not have stays drawn, as it has nothing to hide behind
    func visible(_ s: Setting) -> Bool {
        guard let n = s.needs, let eq = n.firstIndex(of: "=") else { return true }
        let key = String(n[n.startIndex..<eq]), want = String(n[n.index(after: eq)...])
        guard let v = values[key] else { return true }
        return v == want
    }
    func rows(in section: String) -> [Setting] { settings.filter { $0.sectionTitle == section && visible($0) } }
    // The Mac window: Basic settings first, then Advanced settings, each in the list's sections and order
    func sections(basic: Bool) -> [String] {
        settings.filter { $0.basic == basic }.reduce(into: []) { if !$0.contains($1.sectionTitle) { $0.append($1.sectionTitle) } }
    }
    func rows(in section: String, basic: Bool) -> [Setting] { rows(in: section).filter { $0.basic == basic } }
    // A range's label: its percentage, with the size in pixels for the chosen console mode when the list has the base size (the render scale)
    func rangeLabel(_ s: Setting, _ v: Double) -> String {
        let p = "\(Int((v * 100).rounded())) %"
        guard let b = s.base else { return p }
        let size = (values["DM_OPMODE"] ?? "1") == "0" ? b.handheld : b.docked
        guard size.count == 2 else { return p }
        return "\(Int((Double(size[0]) * v).rounded())) × \(Int((Double(size[1]) * v).rounded())) (\(p))"
    }
    var changed: Bool { settings.contains { !fixed.contains($0.env) && !$0.isDefault(values[$0.env] ?? $0.def) } }

    // Reset every launcher choice to the game's defaults on the screen; values fixed by an env file or at launch stay. The saved choices are forgotten when Play applies
    // the defaults (apply() keeps no default), so Quit after Reset keeps them, and a choice a launch override hides is kept
    func reset() {
        for s in settings where !fixed.contains(s.env) { values[s.env] = s.def }
        fputs("launcher: settings reset to the game's defaults\n", stderr)
    }

    // Before the game starts: every setting not set by an env file goes into the environment (DM_LANG replaces the packed language main.m set as a default). Only a choice that
    // differs from the game's default is stored: a setting the player left at its default follows the defaults of a later build (the settings it was tested with)
    @discardableResult func apply() -> Bool {
        if !userLocked && selectedUser != dm_users_selected_index() && dm_users_select_index(selectedUser) == 0 {
            fputs("[RED] launcher: could not select the local user\n", stderr)
            return false
        }
        for s in settings {
            let v = values[s.env] ?? s.def
            if fixed.contains(s.env) { fputs("launcher: \(s.env) kept at \(v) (\(fromLaunch.contains(s.env) ? "launch environment" : "env file"))\n", stderr); continue }
            store(s, v)
            setenv(s.env, v, 1); fputs("launcher: \(s.env)=\(v)\(s.isDefault(v) ? "" : " (changed)")\n", stderr)
        }
        return true
    }
    private func store(_ s: Setting, _ v: String) {
        if !ignoreSaved { if s.isDefault(v) { defaults.removeObject(forKey: "launcher." + s.env) } else { defaults.set(v, forKey: "launcher." + s.env) } }
    }

    // A choice made while the game runs (the iOS in-game menu, GameMenu.swift): saved as Play saves it, so the settings screen shows it at the next start, and put in the environment.
    // A value an env file or the launch fixed stays
    func choose(_ s: Setting, _ v: String) {
        guard !fixed.contains(s.env), s.accepts(v) else { return }
        values[s.env] = v; store(s, v); setenv(s.env, v, 1)
        fputs("launcher: \(s.env)=\(v) (in-game menu)\n", stderr)
    }

    // DM_LAUNCHER=0: no screen, but the saved choices still apply (only DM_SAVED_CHOICES=0 ignores them); nothing is written
    func applySaved() {
        for s in settings where !fixed.contains(s.env) {
            let v = values[s.env] ?? s.def
            if !s.isDefault(v) { setenv(s.env, v, 1); fputs("launcher: \(s.env)=\(v) (saved choice)\n", stderr) }
        }
    }
}
