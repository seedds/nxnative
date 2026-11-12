import SwiftUI

@MainActor final class AppLanguage: ObservableObject {
    static let shared = AppLanguage()
    @Published var code: String { didSet { UserDefaults.standard.set(code,forKey:"DSUMotionLab.language") } }
    init() {
        let saved=UserDefaults.standard.string(forKey:"DSUMotionLab.language")
        let system=Locale.preferredLanguages.first ?? "en"
        code=saved ?? (system.hasPrefix("pt") ? "pt-BR" : (system.hasPrefix("es") ? "es" : (system.hasPrefix("zh") ? "zh-Hant" : "en")))
    }
}

enum Catalog {
    static let translations: [String:[String:String]] = {
        #if SWIFT_PACKAGE
        let bundle=Bundle.module
        #else
        let bundle=Bundle.main
        #endif
        guard let url=bundle.url(forResource:"DSUMotionTranslations",withExtension:"json"),let data=try? Data(contentsOf:url),let rows=try? JSONDecoder().decode([String:[String:String]].self,from:data) else { return [:] }
        return rows
    }()
    static func translate(_ text:String,language:String) -> String {
        guard language != "en" else { return text }
        if let result=translations[text]?[language] { return result }
        // Dynamic strings use numbered placeholders, not concatenated translated fragments.
        for (key,values) in translations where key.contains("{0}") {
            guard let translated=values[language] else { continue }
            let parts=key.components(separatedBy:"{0}")
            guard parts.count==2 else { continue }
            let pattern="^"+NSRegularExpression.escapedPattern(for:parts[0])+"(.+?)"+NSRegularExpression.escapedPattern(for:parts[1])+"$"
            guard let regex=try? NSRegularExpression(pattern:pattern),let match=regex.firstMatch(in:text,range:NSRange(text.startIndex...,in:text)),let range=Range(match.range(at:1),in:text) else { continue }
            return translated.replacingOccurrences(of:"{0}",with:String(text[range]))
        }
        return text
    }
}
struct LocalizedText: View {
    @ObservedObject private var language = AppLanguage.shared
    var text:String
    init(_ text:String) { self.text=text }
    var body: some View { Text(verbatim:Catalog.translate(text,language:language.code)) }
}
@MainActor func L(_ text:String) -> String { Catalog.translate(text,language:AppLanguage.shared.code) }
