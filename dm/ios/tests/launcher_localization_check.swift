// Native bundle lookup with a synthetic translation, plus legacy JSON and missing-key fallback.
import Foundation

@main struct LocalizationCheck {
    static func main() throws {
        let file = try JSONDecoder().decode(SettingsFile.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])))
        let env = Dictionary(uniqueKeysWithValues: file.settings.map { ($0.env, $0) })
        if CommandLine.arguments.contains("--english") {
            precondition(Bundle.main.preferredLocalizations.first == "en", "unsupported locale did not fall back to English")
            precondition(launcherString("Play") == "Play")
            precondition(env["DM_OPMODE"]!.displayLabel() == "Console mode")
            precondition(env["DM_OPMODE"]!.displayOption(["0", "Handheld"]) == "Handheld")
            precondition(env["DM_OPMODE"]!.displaySection() == "Game")
            print("launcher localization: passed")
            return
        }
        precondition(Bundle.main.preferredLocalizations.first == "fr", "fixture language was not selected")
        precondition(launcherString("Play") == "Translated play")
        precondition(launcherString("Unknown button") == "Unknown button")
        precondition(String(format: launcherString("Version %@"), "1.2.3") == "1.2.3 translated version")
        let mode = env["DM_OPMODE"]!
        precondition(mode.displayLabel() == "Translated console mode")
        precondition(mode.displaySection() == "Translated section")
        precondition(mode.displayOption(["0", "Handheld"]) == "Translated handheld")
        precondition(mode.displayOption(["1", "Docked"]) == "Docked") // missing translation uses JSON text
        precondition(mode.displayNote().isEmpty)
        let upscale = env["DM_PRESENT_UPSCALE"]!
        precondition(upscale.displayNote() == "Translated GPU note")
        precondition(env["DM_LANG"]!.displayOption(["ja", "Japanese"]) == "Translated Japanese")
        // A front end's JSON predating the optional keys is still readable, and an unknown future key stays readable too.
        for key in [nil, "setting.FUTURE.label"] as [String?] {
            var row: [String: Any] = ["env": "DM_TEST", "kind": "toggle", "label": "Legacy label", "note": "Legacy note", "default": "0"]
            row["label_key"] = key
            let old = try JSONDecoder().decode(Setting.self, from: JSONSerialization.data(withJSONObject: row))
            precondition(old.displayLabel() == "Legacy label" && old.displayNote() == "Legacy note")
            precondition(old.displaySection() == "Game" && old.onValue == "1" && old.offValue == "0")
        }
        let suite = "org.nxnative.localization-test." + UUID().uuidString
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let model = LauncherModel(file.settings, defaults: defaults)
        // Both headings have the same synthetic translation; grouping and persistence still use raw identities and values.
        precondition(model.sections.contains("Game") && model.sections.contains("Controls"))
        precondition(model.rows(in: "Game").contains { $0.env == "DM_OPMODE" })
        precondition(!model.rows(in: "Controls").contains { $0.env == "DM_OPMODE" })
        model.values["DM_OPMODE"] = "0"
        model.apply()
        precondition(defaults.string(forKey: "launcher.DM_OPMODE") == "0")
        precondition(String(cString: getenv("DM_OPMODE")) == "0")
        precondition(model.values["DM_LANG"] == env["DM_LANG"]!.def) // launcher locale does not select the game language
        print("launcher localization: passed")
    }
}
