import Foundation

public enum CompatibilityDisplayMode: String, CaseIterable, Identifiable {
    case list = "list"
    case grid = "grid"
    
    public var id: String { rawValue }
}
