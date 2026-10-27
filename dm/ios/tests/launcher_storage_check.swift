// The launcher's storage (LauncherSettings.swift LauncherModel), run by test_launcher_storage.py on macOS: only choices that differ from the game's default are
// stored, Reset to tested defaults clears all choices, an env file's values are kept. Arguments: iOS and Mac launcher.json for Breath of the Wild (settings.py, GFXAPI=nvn)
import Foundation
@main struct Check {
    static func main() {
        var failed = 0
        func expect(_ c: Bool, _ what: String) { print((c ? "PASS " : "FAIL ") + what); if !c { failed += 1 } }
        let file = try! JSONDecoder().decode(SettingsFile.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])))
        let suite = "nxnative.launcher-test." + UUID().uuidString
        let d = UserDefaults(suiteName: suite)!
        defer { d.removePersistentDomain(forName: suite) }
        d.set("1", forKey: "launcher.DM_PRESENT_SNAP")   // stored by the old launcher, equal to the default
        d.set("30", forKey: "launcher.DM_FPS_TARGET")    // a changed game setting
        d.set("0", forKey: "launcher.DM_PSO_MTL4")       // a changed performance setting
        d.set("1", forKey: "launcher.DM_FPS_HOLD")       // a value the toggle does not take (on / off): the default
        setenv("DM_ENV_FILE_KEYS", "DM_METAL_HUD", 1); setenv("DM_METAL_HUD", "1", 1)   // set by Documents/env.txt
        let m = LauncherModel(file.settings, defaults: d)
        let scale = file.settings.first { $0.env == "DM_RENDER_SCALE" }!
        expect(["nan", "inf", "-inf", "0.49", "1.01", "invalid"].allSatisfy { !scale.accepts($0) } && scale.accepts("0.5") && scale.accepts("1.00"), "saved ranges reject nonfinite and out-of-range values")
        d.set("nan", forKey: "launcher.DM_RENDER_SCALE")
        expect(LauncherModel(file.settings, defaults: d).values["DM_RENDER_SCALE"] == scale.def, "an invalid saved range loads the default")
        expect(m.values["DM_FPS_TARGET"] == "30" && m.values["DM_PSO_MTL4"] == "0" && m.values["DM_FPS_HOLD"] == "on" && m.values["DM_METAL_HUD"] == "1", "stored choices and the env file are read")
        expect(m.changed, "Reset is offered with changed settings")
        m.values["DM_RENDER_SCALE"] = "1.00"   // the slider's format of the default
        m.apply()
        expect(d.string(forKey: "launcher.DM_PRESENT_SNAP") == nil, "a value equal to the default is not stored (and an old one is dropped)")
        expect(d.string(forKey: "launcher.DM_RENDER_SCALE") == nil && d.string(forKey: "launcher.DM_OPMODE") == nil && d.string(forKey: "launcher.DM_FPS_HOLD") == nil, "defaults are not stored")
        expect(d.string(forKey: "launcher.DM_FPS_TARGET") == "30" && d.string(forKey: "launcher.DM_PSO_MTL4") == "0", "changed choices are stored")
        expect(d.string(forKey: "launcher.DM_METAL_HUD") == nil && String(cString: getenv("DM_METAL_HUD")) == "1", "the env file's value is kept and not stored")
        expect(String(cString: getenv("DM_PSO_MTL4")) == "0" && String(cString: getenv("DM_PRESENT_SNAP")) == "1" && String(cString: getenv("DM_FPS_HOLD")) == "on", "the environment gets every value")
        m.reset()
        expect(d.string(forKey: "launcher.DM_PSO_MTL4") == nil && m.values["DM_PSO_MTL4"] == "1", "Reset clears performance choices and shows the defaults")
        expect(d.string(forKey: "launcher.DM_FPS_TARGET") == nil && m.values["DM_FPS_TARGET"] == file.settings.first { $0.env == "DM_FPS_TARGET" }!.def, "Reset also clears game choices")
        expect(m.values["DM_METAL_HUD"] == "1" && !m.changed, "Reset leaves the env file's value; nothing left to reset")
        // Sub-settings: a row with "needs" is drawn only while the setting it names has that value
        m.values["DM_RUMBLE"] = "1"
        expect(m.rows(in: "Controls").contains { $0.env == "DM_RUMBLE_GAIN" }, "a sub-setting is drawn while its setting is on")
        m.values["DM_RUMBLE"] = "0"
        expect(!m.rows(in: "Controls").contains { $0.env == "DM_RUMBLE_GAIN" }, "a sub-setting is hidden while its setting is off")
        expect(m.rows(in: "Controls").contains { $0.env == "DM_RUMBLE" }, "the setting it depends on stays drawn")
        m.values["DM_RUMBLE"] = "1"
        // DM_LAUNCHER=0 skips the screen, not the saved choices; DM_SAVED_CHOICES=0 ignores them and leaves them stored
        let fps = file.settings.first { $0.env == "DM_FPS_TARGET" }!
        d.set("30", forKey: "launcher.DM_FPS_TARGET"); unsetenv("DM_FPS_TARGET")
        LauncherModel(file.settings, defaults: d).applySaved()
        expect(String(cString: getenv("DM_FPS_TARGET")) == "30" && d.string(forKey: "launcher.DM_FPS_TARGET") == "30", "without the screen, a saved choice goes into the environment and stays saved")
        setenv("DM_SAVED_CHOICES", "0", 1); unsetenv("DM_FPS_TARGET")
        let plain = LauncherModel(file.settings, defaults: d)
        expect(plain.values["DM_FPS_TARGET"] == fps.def, "DM_SAVED_CHOICES=0 starts from the defaults")
        plain.applySaved()
        expect(getenv("DM_FPS_TARGET") == nil, "DM_SAVED_CHOICES=0: nothing saved goes into the environment")
        plain.apply()
        expect(String(cString: getenv("DM_FPS_TARGET")) == fps.def && d.string(forKey: "launcher.DM_FPS_TARGET") == "30", "DM_SAVED_CHOICES=0: Play uses the screen's values and leaves the saved choices")
        plain.reset()
        expect(d.string(forKey: "launcher.DM_FPS_TARGET") == "30", "DM_SAVED_CHOICES=0: Reset leaves the saved choices too")
        unsetenv("DM_SAVED_CHOICES")
        let mac = try! JSONDecoder().decode(SettingsFile.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[2])))
        d.removePersistentDomain(forName: suite)
        for s in mac.settings { unsetenv(s.env) }
        setenv("DM_ENV_FILE_KEYS", "", 1)
        let mm = LauncherModel(mac.settings, defaults: d, fromEnvironment: true)
        expect(mm.sections(for: .basic) == ["Game", "Controls"] && mm.sections(for: .advanced) == ["Display", "Controller", "Overlay", "Performance"], "Mac sections use schema 1 row levels")
        let old = try! JSONDecoder().decode(Setting.self, from: Data(#"{"env":"DM_OLD","kind":"toggle","label":"Old","note":"","default":"0"}"#.utf8))
        expect(old.rowLevel == .basic && old.sectionTitle == "Game", "older settings without a level still decode")
        let invalid = Data(#"{"env":"DM_BAD","kind":"toggle","label":"Bad","note":"","default":"0","level":"expert"}"#.utf8)
        expect((try? JSONDecoder().decode(Setting.self, from: invalid)) == nil, "unknown row levels are rejected")
        expect(mm.values["DM_FULLSCREEN"] == "" && mac.api == "NVN", "Display mode defaults to Window")
        mm.apply()
        expect(getenv("DM_FULLSCREEN") == nil, "Window unsets DM_FULLSCREEN")
        mm.values["DM_FULLSCREEN"] = "1"; mm.apply()
        expect(String(cString: getenv("DM_FULLSCREEN")) == "1" && d.string(forKey: "launcher.DM_FULLSCREEN") == "1", "Fullscreen sets it and is stored")
        expect(LauncherModel(mac.settings, defaults: d).values["DM_FULLSCREEN"] == "1", "stored Fullscreen is read back")
        let macScale = mac.settings.first { $0.env == "DM_RENDER_SCALE" }!
        expect(macScale.base?["docked"] == [1600, 900] && macScale.base?["handheld"] == [1280, 720], "render pixels decode from schema 1 base")
        mm.values["DM_OPMODE"] = "1"
        expect(mm.rangeLabel(macScale, 1) == "1600 × 900 (100 %)" && mm.rangeLabel(macScale, 0.5) == "800 × 450 (50 %)", "docked pixels scale with the slider")
        mm.values["DM_OPMODE"] = "0"
        expect(mm.rangeLabel(macScale, 1) == "1280 × 720 (100 %)" && mm.rangeLabel(macScale, 0.5) == "640 × 360 (50 %)", "handheld pixels scale with the slider")
        mm.values["DM_OPMODE"] = "2"
        expect(mm.rangeLabel(macScale, 0.5) == "50 %", "an unknown console mode shows the percentage")
        mm.values["DM_RUMBLE"] = "0"
        expect(!mm.sections(for: .advanced).contains("Controller") && mm.rows(in: "Controller").isEmpty, "rumble off hides its sub-settings and the empty Mac section")
        mm.values["DM_RUMBLE"] = "1"
        expect(mm.rows(in: "Controller").map(\.env) == ["DM_RUMBLE_GAIN", "DM_RUMBLE_MIN"], "rumble on restores both sub-settings")
        for s in mac.settings { unsetenv(s.env) }
        d.set("1", forKey: "launcher.DM_OPMODE"); setenv("DM_OPMODE", "0", 1)
        let locked = LauncherModel(mac.settings, defaults: d, fromEnvironment: true)
        expect(locked.fromFile.contains("DM_OPMODE") && locked.values["DM_OPMODE"] == "0", "the Mac launch environment wins over stored choices")
        locked.applySaved(); locked.apply(); locked.reset()
        expect(String(cString: getenv("DM_OPMODE")) == "0" && locked.values["DM_OPMODE"] == "0", "Mac launch overrides survive skipped UI, Play and Reset")
        failed += checkLauncherLayout(mm, defaults: d)
        print("launcher storage: \(failed) failed")
        exit(failed == 0 ? 0 : 1)
    }
}
