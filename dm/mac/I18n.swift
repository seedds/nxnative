// Display text for the Mac launcher only. Game language and saved setting values are independent.
import Foundation
import Combine

final class MacI18n: ObservableObject {
    enum Language: String, CaseIterable {
        case english = "en", simplifiedChinese = "zh-Hans"

        var name: String {
            switch self {
            case .english: return "English"
            case .simplifiedChinese: return "简体中文"   // language self-names remain recognizable in either UI language
            }
        }
    }

    static let shared = MacI18n()
    @Published var language: Language

    init(bundle: Bundle = .main) {
        // Each launch starts with the system's best supported language; manual changes last for this launch.
        language = bundle.preferredLocalizations.first.flatMap(Language.init(rawValue:)) ?? .english
    }

    var locale: Locale { Locale(identifier: language.rawValue) }

    func localized(_ key: String, bundle: Bundle = .main) -> String {
        let catalog: Bundle
        if let path = bundle.path(forResource: language.rawValue, ofType: "lproj"), let selected = Bundle(path: path) {
            catalog = selected
        } else {
            catalog = bundle
        }
        return catalog.localizedString(forKey: key, value: key, table: "Launcher")
    }

    static func text(_ key: String, bundle: Bundle = .main) -> String {
        shared.localized(key, bundle: bundle)
    }

    static func format(_ key: String, _ arguments: CVarArg...) -> String {
        String(format: text(key), locale: shared.locale, arguments: arguments)
    }

    private static func when(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = shared.locale
        formatter.doesRelativeDateFormatting = true
        formatter.dateStyle = .short
        formatter.timeStyle = .short
        return formatter.string(from: date)
    }

    static func localSaveLabel(_ report: SaveSyncReport) -> String {
        format("This Mac's: %@, %@", when(report.localSaved),
               ByteCountFormatter.string(fromByteCount: report.localSize, countStyle: .file))
    }

    static func cloudSaveLabel(_ report: SaveSyncReport) -> String {
        format("The %@'s in iCloud: %@, %@", text(report.cloudDeviceKind), when(report.cloudSaved),
               ByteCountFormatter.string(fromByteCount: report.cloudSize, countStyle: .file))
    }

    static func chooseSaveMessage() -> String {
        text("Both this Mac's save and the one in iCloud changed since they were last the same. The newer one is not always the one with more progress: a new game saved a minute later is newer. The one you don't pick is kept in the save's backups folder.")
    }

    static func unreadableSaveMessage() -> String {
        format("iCloud didn't deliver this game's save in %d seconds. If you play this Mac's save now, it is not sent to iCloud until it has been compared with the cloud's at a later start.", Int(SaveSyncReport.waitMs / 1000))
    }
}
