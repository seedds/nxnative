import Foundation

public enum AppIconAppearanceMode: String, CaseIterable, Identifiable, Hashable {
    case light = "Light"
    case dark = "Dark"
    case mono = "Monochrome"
    case auto = "Auto (System)"
    
    public var id: String { rawValue }
}
