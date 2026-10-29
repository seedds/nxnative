import Foundation
import AppKit

public enum MenuBarIconStyle: String, CaseIterable, Identifiable, Hashable, Codable {
    case symbol = "symbol"         // Default: official white Joy-Con symbol without background
    case dark = "dark"             // Official dark squircle app icon
    case lightColor = "lightColor" // Official light with vibrant gradient color squircle app icon
    
    public var id: String { rawValue }
    
    public var isTemplate: Bool {
        switch self {
        case .symbol:
            return true
        case .dark, .lightColor:
            return false
        }
    }
}
