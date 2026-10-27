import Foundation

public enum CompatibilityFocusArea: Equatable {
    case filters
    case games
}

public enum CompatibilityButtonFocus: Int, Equatable {
    case notes = 0
    case portGame = 1
}
