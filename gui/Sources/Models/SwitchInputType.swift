import Foundation

public enum SwitchButton: String, CaseIterable, Codable, Identifiable {
    case a = "A"
    case b = "B"
    case x = "X"
    case y = "Y"
    case dpadUp = "D-Pad Up"
    case dpadDown = "D-Pad Down"
    case dpadLeft = "D-Pad Left"
    case dpadRight = "D-Pad Right"
    case l = "L"
    case r = "R"
    case zl = "ZL"
    case zr = "ZR"
    case plus = "+"
    case minus = "−"
    case leftStickClick = "L3"
    case rightStickClick = "R3"
    case home = "Home"
    case capture = "Capture"

    public var id: String { rawValue }

    public var bitmask: UInt64 {
        switch self {
        case .a: return 1 << 0
        case .b: return 1 << 1
        case .x: return 1 << 2
        case .y: return 1 << 3
        case .leftStickClick: return 1 << 4
        case .rightStickClick: return 1 << 5
        case .l: return 1 << 6
        case .r: return 1 << 7
        case .zl: return 1 << 8
        case .zr: return 1 << 9
        case .plus: return 1 << 10
        case .minus: return 1 << 11
        case .dpadLeft: return 1 << 12
        case .dpadUp: return 1 << 13
        case .dpadRight: return 1 << 14
        case .dpadDown: return 1 << 15
        case .home, .capture: return 0
        }
    }

    public var displayName: String {
        switch self {
        case .a: return "A Button"
        case .b: return "B Button"
        case .x: return "X Button"
        case .y: return "Y Button"
        case .dpadUp: return "D-Pad Up"
        case .dpadDown: return "D-Pad Down"
        case .dpadLeft: return "D-Pad Left"
        case .dpadRight: return "D-Pad Right"
        case .l: return "L Bumper"
        case .r: return "R Bumper"
        case .zl: return "ZL Trigger"
        case .zr: return "ZR Trigger"
        case .plus: return "+ Plus"
        case .minus: return "− Minus"
        case .leftStickClick: return "Left Stick Click (L3)"
        case .rightStickClick: return "Right Stick Click (R3)"
        case .home: return "Home Button"
        case .capture: return "Capture Button"
        }
    }

    public var isLeftSide: Bool {
        switch self {
        case .l, .zl, .minus, .capture, .leftStickClick, .dpadUp, .dpadDown, .dpadLeft, .dpadRight:
            return true
        case .r, .zr, .plus, .home, .rightStickClick, .a, .b, .x, .y:
            return false
        }
    }

    public var defaultKeyCode: UInt16 {
        switch self {
        case .a: return 38          // J
        case .b: return 40          // K
        case .x: return 32          // U
        case .y: return 34          // I
        case .leftStickClick: return 3 // F
        case .rightStickClick: return 5 // G
        case .l: return 12          // Q
        case .r: return 14          // E
        case .zl: return 6          // Z
        case .zr: return 8          // C
        case .plus: return 36       // Return
        case .minus: return 48      // Tab
        case .dpadLeft: return 123  // Left Arrow
        case .dpadUp: return 126    // Up Arrow
        case .dpadRight: return 124 // Right Arrow
        case .dpadDown: return 125  // Down Arrow
        case .home: return 53       // Escape
        case .capture: return 49    // Space
        }
    }

    public var defaultGamepadElement: String {
        switch self {
        case .a: return "buttonB"               // Nintendo A is East (GC buttonB)
        case .b: return "buttonA"               // Nintendo B is South (GC buttonA)
        case .x: return "buttonY"               // Nintendo X is North (GC buttonY)
        case .y: return "buttonX"               // Nintendo Y is West (GC buttonX)
        case .l: return "leftShoulder"
        case .r: return "rightShoulder"
        case .zl: return "leftTrigger"
        case .zr: return "rightTrigger"
        case .plus: return "buttonMenu"
        case .minus: return "buttonOptions"
        case .leftStickClick: return "leftThumbstickButton"
        case .rightStickClick: return "rightThumbstickButton"
        case .dpadUp: return "dpad.up"
        case .dpadDown: return "dpad.down"
        case .dpadLeft: return "dpad.left"
        case .dpadRight: return "dpad.right"
        case .home: return "buttonHome"
        case .capture: return "buttonOptions"
        }
    }
}
