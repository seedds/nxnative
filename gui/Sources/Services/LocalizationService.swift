import Foundation
import Combine

public final class LocalizationService: ObservableObject {
    public static let shared = LocalizationService()
    
    @Published public var currentLanguage: AppLanguage = .system
    
    private init() {}
    
    public var effectiveLanguage: AppLanguage {
        if currentLanguage == .system {
            return AppLanguage.resolve()
        }
        return currentLanguage
    }
    
    public func t(_ key: String, _ args: CVarArg...) -> String {
        format(key: key, arguments: args)
    }
    
    public func format(key: String, arguments: [CVarArg]) -> String {
        let dict = Translations.dictionary(for: effectiveLanguage)
        let formatStr = dict[key] ?? Translations.english[key] ?? key
        if arguments.isEmpty {
            return formatStr
        }
        return String(format: formatStr, arguments: arguments)
    }
}

public enum L10n {
    public static func t(_ key: String, _ args: CVarArg...) -> String {
        LocalizationService.shared.format(key: key, arguments: args)
    }
}
