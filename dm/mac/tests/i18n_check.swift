// A native bundle fixture for test_mac_i18n.py. No game or visible window is needed.
import Foundation
import Combine
import SwiftUI

@main struct I18nCheck {
    static func main() {
        var failed = 0
        func expect(_ condition: Bool, _ message: String) {
            print((condition ? "PASS " : "FAIL ") + message)
            if !condition { failed += 1 }
        }
        let standard = UserDefaults.standard
        if let index = CommandLine.arguments.firstIndex(of: "--select-language") {
            let raw = CommandLine.arguments[index + 1]
            standard.removeObject(forKey: "window.language")
            MacI18n.shared.language = MacI18n.Language(rawValue: raw)!
            expect(MacI18n.text("Play") == (raw == "zh-Hans" ? "开始游戏" : "Play"), "the selected language is used for this launch")
            expect(standard.object(forKey: "window.language") == nil, "the selected language is not saved")
            standard.synchronize()
            print("mac i18n: \(failed) failed")
            exit(failed == 0 ? 0 : 1)
        }
        if let index = CommandLine.arguments.firstIndex(of: "--initial-language") {
            let chinese = CommandLine.arguments[index + 1] == "zh"
            expect(MacI18n.shared.language == (chinese ? .simplifiedChinese : .english), "a new process chooses its supported system language")
            expect(MacI18n.text("Play") == (chinese ? "开始游戏" : "Play"), "a previous manual selection does not change the next launch")
            expect(String(cString: getenv("DM_LANG")) == "ja", "the initial interface language leaves the game language unchanged")
            print("mac i18n: \(failed) failed")
            exit(failed == 0 ? 0 : 1)
        }
        let chinese = CommandLine.arguments.last == "zh"
        standard.set(chinese ? "en" : "zh-Hans", forKey: "window.language")
        defer { standard.removeObject(forKey: "window.language"); standard.synchronize() }
        expect(MacI18n.shared.language == (chinese ? .simplifiedChinese : .english), "the initial interface language ignores an old saved preference")
        expect(Set(MacI18n.Language.allCases.map(\.rawValue)) == Set(["en", "zh-Hans"]), "the interface picker contains only the supported languages")
        expect(MacI18n.text("Play") == (chinese ? "开始游戏" : "Play"), "the app's preferred language selects its catalog")
        expect(MacI18n.text("General") == (chinese ? "通用" : "General"), "the interface language is in General")
        expect(MacI18n.text("Applies only to this settings window.") == (chinese ? "仅影响当前设置界面。" : "Applies only to this settings window."), "the language description identifies this settings window")
        expect(MacI18n.text("Console mode") == (chinese ? "主机模式" : "Console mode"), "the console mode label is localized")
        expect(MacI18n.text("Docked") == (chinese ? "TV 模式" : "Docked"), "the docked mode uses its display name")
        expect(MacI18n.text("Handheld") == (chinese ? "掌机模式" : "Handheld"), "the handheld mode uses its display name")
        expect(MacI18n.text("a future setting") == "a future setting", "an unknown key keeps the source text")
        expect(MacI18n.format("Version %@", "2.3") == (chinese ? "版本 2.3" : "Version 2.3"), "format arguments are preserved")
        let bluetooth = Bundle.main.object(forInfoDictionaryKey: "NSBluetoothAlwaysUsageDescription") as? String
        expect(bluetooth?.hasPrefix(chinese ? "通过蓝牙连接" : "Connect Switch 2") == true, "the Bluetooth permission purpose follows the app language")
        if let path = Bundle.main.path(forResource: "en", ofType: "lproj"), let english = Bundle(path: path) {
            expect(MacI18n.text("Play", bundle: english) == "Play", "the explicit bundle argument is honored")
        } else {
            expect(false, "the bundle contains its English fallback catalog")
        }

        let data = Data(#"{"settings":[{"env":"DM_OPMODE","kind":"choice","section":"Game","label":"Console mode","note":"","default":"1","options":[["1","Docked"],["0","Handheld"]]},{"env":"DM_LANG","kind":"choice","section":"Game","label":"Game language","note":"","default":"ja","options":[["ja","Japanese"],["en-US","American English"]]}]}"#.utf8)
        let file = try! JSONDecoder().decode(SettingsFile.self, from: data)
        let suite = "nxnative.i18n-test." + UUID().uuidString
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let model = LauncherModel(file.settings, defaults: defaults)
        let mode = file.settings[0]
        let plainRow = SettingRow(s: mode, m: model)
        let localizedRow = SettingRow(s: mode, m: model, displayText: { MacI18n.text($0) })
        expect(plainRow.displayText(mode.label) == "Console mode", "the shared row keeps its default display text")
        expect(localizedRow.displayText(mode.label) == (chinese ? "主机模式" : "Console mode"), "the Mac row uses the supplied display text")
        expect(mode.options == [["1", "Docked"], ["0", "Handheld"]], "localization leaves decoded choice identifiers and source labels intact")
        model.binding(mode).wrappedValue = "0"
        model.values["DM_LANG"] = "en-US"
        model.apply()
        expect(model.sections == ["Game"], "section identity remains the source value")
        expect(defaults.string(forKey: "launcher.DM_OPMODE") == "0" && defaults.string(forKey: "launcher.DM_LANG") == "en-US", "saved choices keep their raw environment values")
        expect(String(cString: getenv("DM_OPMODE")) == "0" && String(cString: getenv("DM_LANG")) == "en-US", "the game's environment uses raw values")
        expect(MacI18n.text("Play") == (chinese ? "开始游戏" : "Play"), "changing game language does not change the launcher")

        let report = SaveSyncReport(line: "sync=1 conflict=1 cloud=1 local=aaa chash=bbb cdevice=Mac16,1 saved=1700000000 csaved=1700003600 size=1234567 csize=7654321")!
        let local = MacI18n.localSaveLabel(report), cloud = MacI18n.cloudSaveLabel(report)
        expect(local.hasPrefix(chinese ? "本机存档：" : "This Mac's: "), "the local save has a localized label")
        expect(cloud.hasPrefix(chinese ? "iCloud 中的 Mac 存档：" : "The Mac's in iCloud: "), "the cloud save identifies its device")
        let date = DateFormatter(); date.locale = MacI18n.shared.locale; date.doesRelativeDateFormatting = true; date.dateStyle = .short; date.timeStyle = .short
        expect(local.contains(date.string(from: report.localSaved)) && cloud.contains(date.string(from: report.cloudSaved)), "both save times survive formatting")
        expect(local.contains(ByteCountFormatter.string(fromByteCount: report.localSize, countStyle: .file)) && cloud.contains(ByteCountFormatter.string(fromByteCount: report.cloudSize, countStyle: .file)), "both save sizes survive formatting")
        var unknownDevice = report; unknownDevice.cloudDevice = ""
        expect(MacI18n.cloudSaveLabel(unknownDevice).contains(chinese ? "其他设备" : "another device"), "an unknown cloud device is localized")
        unknownDevice.cloudDevice = "TestDevice9,7"
        expect(MacI18n.cloudSaveLabel(unknownDevice).contains("TestDevice9,7"), "an unknown device model is preserved")
        let conflict = MacI18n.chooseSaveMessage(), unreadable = MacI18n.unreadableSaveMessage()
        expect(conflict.hasPrefix(chinese ? "本机和 iCloud" : "Both this Mac's save"), "the conflict explanation is localized")
        expect(unreadable.contains("60") && unreadable.hasPrefix(chinese ? "iCloud 未能" : "iCloud didn't"), "the cloud timeout is formatted in the selected language")
        expect(![local, cloud, conflict, unreadable].contains { $0.contains("%@") || $0.contains("%d") }, "save messages contain no unexpanded format arguments")
        expect(SaveSyncReport.step(report) == .choose && report.localChoice == "local:aaa" && report.cloudChoice == "cloud:bbb", "localized save messages leave conflict handling and choices unchanged")

        let values = model.values
        let saved = defaults.persistentDomain(forName: suite) as NSDictionary?
        standard.removeObject(forKey: "window.language")
        var changes = 0
        let observation = MacI18n.shared.objectWillChange.sink { changes += 1 }
        let selections: [MacI18n.Language] = chinese ? [.english, .simplifiedChinese] : [.simplifiedChinese, .english]
        for language in selections {
            let label = language == .simplifiedChinese ? "开始游戏" : "Play"
            let modeLabel = language == .simplifiedChinese ? "主机模式" : "Console mode"
            MacI18n.shared.language = language
            expect(MacI18n.text("Play") == label && localizedRow.displayText(mode.label) == modeLabel, "changing the interface language immediately updates display text")
            expect(standard.object(forKey: "window.language") == nil, "changing the interface language does not save a preference")
            expect(model.values == values && (defaults.persistentDomain(forName: suite) as NSDictionary?) == saved, "changing the interface language keeps current and saved game settings")
            expect(String(cString: getenv("DM_LANG")) == "en-US", "changing the interface language does not set DM_LANG")
        }
        expect(changes == 2, "language changes notify the views observing the shared model")
        observation.cancel()

        let automatic = MacI18n()
        expect(automatic.language == (chinese ? .simplifiedChinese : .english), "a fresh language model chooses the supported system language")
        automatic.language = .simplifiedChinese
        expect(automatic.localized("Play") == "开始游戏", "an independent language model applies its current choice")
        if let path = Bundle.main.path(forResource: "en", ofType: "lproj"), let english = Bundle(path: path) {
            expect(automatic.localized("Play", bundle: english) == "Play", "an explicit resource bundle is honored even with a forced interface language")
        }
        print("mac i18n: \(failed) failed")
        exit(failed == 0 ? 0 : 1)
    }
}
