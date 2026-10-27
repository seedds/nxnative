import Foundation

public enum GamepadBrand: String, CaseIterable, Identifiable {
    case nintendoSwitch = "Nintendo Switch"
    case xbox = "Xbox Series / One"
    case playstation = "PlayStation DualShock / DualSense"
    case generic = "8BitDo / Generic Gamepad"
    
    public var id: String { rawValue }
    
    public var confirmButtonName: String {
        switch self {
        case .nintendoSwitch: return "A (Confirm)"
        case .xbox: return "A (Confirm)"
        case .playstation: return "✕ (Confirm)"
        case .generic: return "A (Confirm)"
        }
    }
    
    public var cancelButtonName: String {
        switch self {
        case .nintendoSwitch: return "B (Back)"
        case .xbox: return "B (Back)"
        case .playstation: return "○ (Back)"
        case .generic: return "B (Back)"
        }
    }
    
    public var secondaryButtonName: String {
        switch self {
        case .nintendoSwitch: return "X (Settings)"
        case .xbox: return "X (Settings)"
        case .playstation: return "□ (Settings)"
        case .generic: return "X (Settings)"
        }
    }
    
    public var contextButtonName: String {
        switch self {
        case .nintendoSwitch: return "Y (Save Folder)"
        case .xbox: return "Y (Save Folder)"
        case .playstation: return "△ (Save Folder)"
        case .generic: return "Y (Save Folder)"
        }
    }
    
    public var shoulderTabsPrompt: String {
        switch self {
        case .nintendoSwitch: return "L / R"
        case .xbox: return "LB / RB"
        case .playstation: return "L1 / R1"
        case .generic: return "L / R"
        }
    }
    
    public var leftShoulderName: String {
        switch self {
        case .nintendoSwitch: return "L"
        case .xbox: return "LB"
        case .playstation: return "L1"
        case .generic: return "L"
        }
    }
    
    public var rightShoulderName: String {
        switch self {
        case .nintendoSwitch: return "R"
        case .xbox: return "RB"
        case .playstation: return "R1"
        case .generic: return "R"
        }
    }
    
    public var optionsButtonName: String {
        switch self {
        case .nintendoSwitch: return "− (Options)"
        case .xbox: return "⧉ (Options)"
        case .playstation: return "Options"
        case .generic: return "Select"
        }
    }
}

public enum GamepadAction: Hashable {
    case navigateUp
    case navigateDown
    case navigateLeft
    case navigateRight
    case confirm           // A or Cross
    case cancel            // B or Circle
    case secondaryAction   // X or Square
    case contextAction     // Y or Triangle
    case tabPrevious       // Left Shoulder (L / LB)
    case tabNext           // Right Shoulder (R / RB)
    case menu              // Plus / Start / Menu
    case options           // Minus / View / Options
}
