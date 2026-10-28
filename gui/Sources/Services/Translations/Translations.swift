import Foundation

public enum Translations {
    public static func dictionary(for language: AppLanguage) -> [String: String] {
        switch language {
        case .system:
            let resolved = AppLanguage.resolve()
            return dictionary(for: resolved)
        case .en:
            return english
        case .ptBR:
            return portuguese
        case .es:
            return spanish
        case .ja:
            return japanese
        case .fr:
            return french
        case .de:
            return german
        case .it:
            return italian
        case .zhHans:
            return chineseSimplified
        case .zhHant:
            return chineseTraditional
        case .ko:
            return korean
        case .pl:
            return polish
        case .ru:
            return russian
        case .sv:
            return swedish
        }
    }
}
