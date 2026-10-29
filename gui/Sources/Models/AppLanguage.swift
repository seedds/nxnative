import Foundation

public enum AppLanguage: String, CaseIterable, Identifiable, Codable {
    case system = "system"
    case en = "en"
    case ptBR = "pt-BR"
    case ptPT = "pt-PT"
    case es = "es"
    case ja = "ja"
    case fr = "fr"
    case de = "de"
    case it = "it"
    case zhHans = "zh-Hans"
    case zhHant = "zh-Hant"
    case ko = "ko"
    case pl = "pl"
    case ru = "ru"
    case sv = "sv"
    
    public var id: String { rawValue }

    /// "System Default" first, then the remaining languages by native name (locale-aware).
    public static var sortedForDisplay: [AppLanguage] {
        let rest = allCases.filter { $0 != .system }
            .sorted { $0.nativeName.localizedStandardCompare($1.nativeName) == .orderedAscending }
        return [.system] + rest
    }
    
    public var displayName: String {
        switch self {
        case .system: return "System Default"
        case .en: return "English"
        case .ptBR: return "Portuguese (Brazil)"
        case .ptPT: return "Portuguese (Portugal)"
        case .es: return "Spanish"
        case .ja: return "Japanese"
        case .fr: return "French"
        case .de: return "German"
        case .it: return "Italian"
        case .zhHans: return "Chinese (Simplified)"
        case .zhHant: return "Chinese (Traditional)"
        case .ko: return "Korean"
        case .pl: return "Polish"
        case .ru: return "Russian"
        case .sv: return "Swedish"
        }
    }
    
    public var nativeName: String {
        switch self {
        case .system: return "System Default"
        case .en: return "English (US)"
        case .ptBR: return "Português (Brasil)"
        case .ptPT: return "Português (Portugal)"
        case .es: return "Español"
        case .ja: return "日本語"
        case .fr: return "Français"
        case .de: return "Deutsch"
        case .it: return "Italiano"
        case .zhHans: return "简体中文"
        case .zhHant: return "繁體中文"
        case .ko: return "한국어"
        case .pl: return "Polski"
        case .ru: return "Русский"
        case .sv: return "Svenska"
        }
    }
    
    public var flag: String {
        switch self {
        case .system: return "🌐"
        case .en: return "🇺🇸"
        case .ptBR: return "🇧🇷"
        case .ptPT: return "🇵🇹"
        case .es: return "🇪🇸"
        case .ja: return "🇯🇵"
        case .fr: return "🇫🇷"
        case .de: return "🇩🇪"
        case .it: return "🇮🇹"
        case .zhHans: return "🇨🇳"
        case .zhHant: return "🇭🇰"
        case .ko: return "🇰🇷"
        case .pl: return "🇵🇱"
        case .ru: return "🇷🇺"
        case .sv: return "🇸🇪"
        }
    }
    
    public static func resolve(locale: Locale = Locale.current) -> AppLanguage {
        let id = locale.identifier.lowercased().replacingOccurrences(of: "_", with: "-")
        if id.starts(with: "pt-pt") { return .ptPT }
        if id.starts(with: "pt") { return .ptBR }
        if id.starts(with: "es") { return .es }
        if id.starts(with: "ja") { return .ja }
        if id.starts(with: "fr") { return .fr }
        if id.starts(with: "de") { return .de }
        if id.starts(with: "it") { return .it }
        if id.starts(with: "zh-hant") || id.starts(with: "zh-tw") || id.starts(with: "zh-hk") { return .zhHant }
        if id.starts(with: "zh") { return .zhHans }
        if id.starts(with: "ko") { return .ko }
        if id.starts(with: "pl") { return .pl }
        if id.starts(with: "ru") { return .ru }
        if id.starts(with: "sv") { return .sv }
        if id.starts(with: "en") { return .en }
        
        let preferred = Locale.preferredLanguages
        for pref in preferred {
            let lower = pref.lowercased().replacingOccurrences(of: "_", with: "-")
            if lower.starts(with: "pt-pt") { return .ptPT }
            if lower.starts(with: "pt") { return .ptBR }
            if lower.starts(with: "es") { return .es }
            if lower.starts(with: "ja") { return .ja }
            if lower.starts(with: "fr") { return .fr }
            if lower.starts(with: "de") { return .de }
            if lower.starts(with: "it") { return .it }
            if lower.starts(with: "zh-hant") || lower.starts(with: "zh-tw") || lower.starts(with: "zh-hk") { return .zhHant }
            if lower.starts(with: "zh") { return .zhHans }
            if lower.starts(with: "ko") { return .ko }
            if lower.starts(with: "pl") { return .pl }
            if lower.starts(with: "ru") { return .ru }
            if lower.starts(with: "sv") { return .sv }
            if lower.starts(with: "en") { return .en }
        }
        
        return .en
    }
}
