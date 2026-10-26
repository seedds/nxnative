import Foundation

public enum AppIconAppearanceMode: String, CaseIterable, Identifiable, Hashable {
    case auto = "Auto (System)"
    case light = "Light"
    case dark = "Dark"
    case mono = "Monochrome"
    
    public var id: String { rawValue }
}
