import Foundation

public struct ControllerProfile: Codable, Identifiable, Equatable {
    public var id: UUID
    public var name: String
    public var isDefault: Bool
    public var targetDeviceName: String
    public var gamepadBindings: [String: SwitchButton]
    public var keyboardBindings: [UInt16: SwitchButton]
    public var keyboardMovement: [String: UInt16]
    public var analogDeadzone: Double

    public init(
        id: UUID = UUID(),
        name: String,
        isDefault: Bool = false,
        targetDeviceName: String = "All Gamepads",
        gamepadBindings: [String: SwitchButton],
        keyboardBindings: [UInt16: SwitchButton],
        keyboardMovement: [String: UInt16] = ["up": 13, "left": 0, "down": 1, "right": 2],
        analogDeadzone: Double = 0.10
    ) {
        self.id = id
        self.name = name
        self.isDefault = isDefault
        self.targetDeviceName = targetDeviceName
        self.gamepadBindings = gamepadBindings
        self.keyboardBindings = keyboardBindings
        self.keyboardMovement = keyboardMovement
        self.analogDeadzone = analogDeadzone
    }

    // MARK: - Factory Presets

    public static var nintendoDefault: ControllerProfile {
        var gp: [String: SwitchButton] = [:]
        for btn in SwitchButton.allCases {
            gp[btn.defaultGamepadElement] = btn
        }

        var kb: [UInt16: SwitchButton] = [:]
        for btn in SwitchButton.allCases {
            kb[btn.defaultKeyCode] = btn
        }

        return ControllerProfile(
            name: "Nintendo Switch (Default)",
            isDefault: true,
            targetDeviceName: "Nintendo Switch Pro Controller",
            gamepadBindings: gp,
            keyboardBindings: kb
        )
    }

    public static var xboxLayout: ControllerProfile {
        var p = ControllerProfile.nintendoDefault
        p.id = UUID()
        p.name = "Xbox / PC Controller"
        p.isDefault = false
        p.targetDeviceName = "Xbox Wireless Controller"
        // On Xbox: Physical A (South) is Confirm/A in games, Physical B (East) is Cancel/B
        // Physical X (West) is Y, Physical Y (North) is X
        p.gamepadBindings["buttonA"] = .a
        p.gamepadBindings["buttonB"] = .b
        p.gamepadBindings["buttonX"] = .y
        p.gamepadBindings["buttonY"] = .x
        return p
    }

    public static var playstationLayout: ControllerProfile {
        var p = ControllerProfile.nintendoDefault
        p.id = UUID()
        p.name = "PlayStation DualSense"
        p.isDefault = false
        p.targetDeviceName = "DualSense / DualShock 4"
        p.gamepadBindings["buttonA"] = .b // Cross -> B (Standard Switch Confirm or Japanese)
        p.gamepadBindings["buttonB"] = .a // Circle -> A
        p.gamepadBindings["buttonX"] = .y // Square -> Y
        p.gamepadBindings["buttonY"] = .x // Triangle -> X
        return p
    }

    // MARK: - Quick Face Swap (Enhancement 2: Nintendo <-> Xbox/PC layout)

    public mutating func swapFaceButtons() {
        let oldA = gamepadBindings["buttonB"]
        let oldB = gamepadBindings["buttonA"]
        let oldX = gamepadBindings["buttonY"]
        let oldY = gamepadBindings["buttonX"]

        gamepadBindings["buttonB"] = oldB
        gamepadBindings["buttonA"] = oldA
        gamepadBindings["buttonY"] = oldY
        gamepadBindings["buttonX"] = oldX
    }

    // MARK: - Export for Ported Games (input_mapping.json)

    public func exportDictionary() -> [String: Any] {
        var gpExport: [String: String] = [:]
        for (k, v) in gamepadBindings {
            gpExport[k] = v.rawValue
        }

        var kbExport: [String: String] = [:]
        for (k, v) in keyboardBindings {
            kbExport[String(k)] = v.rawValue
        }

        var movExport: [String: Int] = [:]
        for (k, v) in keyboardMovement {
            movExport[k] = Int(v)
        }

        return [
            "name": name,
            "analogDeadzone": analogDeadzone,
            "gamepad": gpExport,
            "keyboard": kbExport,
            "keyboardMovement": movExport
        ]
    }
}
