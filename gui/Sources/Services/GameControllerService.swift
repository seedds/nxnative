import Foundation
import GameController

public final class GameControllerService: ObservableObject {
    public static let shared = GameControllerService()
    
    @Published public var isGamepadConnected: Bool = false
    @Published public var activeControllerName: String = "No Gamepad Connected"
    @Published public var activeBrand: GamepadBrand = .generic
    
    public var onAction: ((GamepadAction) -> Void)?
    
    private var lastNavTime: TimeInterval = 0
    private let navCooldown: TimeInterval = 0.20
    private let stickDeadzone: Float = 0.35
    
    private init() {
        setupObservers()
        refreshControllers()
    }
    
    private func setupObservers() {
        NotificationCenter.default.addObserver(
            forName: .GCControllerDidConnect,
            object: nil,
            queue: .main
        ) { [weak self] notif in
            self?.refreshControllers()
        }
        
        NotificationCenter.default.addObserver(
            forName: .GCControllerDidDisconnect,
            object: nil,
            queue: .main
        ) { [weak self] notif in
            self?.refreshControllers()
        }
    }
    
    public func refreshControllers() {
        let controllers = GCController.controllers()
        guard let controller = controllers.first else {
            isGamepadConnected = false
            activeControllerName = "No Gamepad Connected"
            activeBrand = .generic
            return
        }
        
        isGamepadConnected = true
        activeControllerName = controller.vendorName ?? "Gamepad"
        activeBrand = detectBrand(from: activeControllerName)
        
        bindController(controller)
    }
    
    private func detectBrand(from vendor: String) -> GamepadBrand {
        let lower = vendor.lowercased()
        if lower.contains("switch") || lower.contains("joy-con") || lower.contains("nintendo") {
            return .nintendoSwitch
        } else if lower.contains("xbox") || lower.contains("microsoft") {
            return .xbox
        } else if lower.contains("dualsense") || lower.contains("dualshock") || lower.contains("sony") || lower.contains("playstation") {
            return .playstation
        }
        return .generic
    }
    
    private func bindController(_ controller: GCController) {
        guard let gamepad = controller.extendedGamepad else { return }
        
        // D-Pad
        gamepad.dpad.up.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.navigateUp) }
        }
        gamepad.dpad.down.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.navigateDown) }
        }
        gamepad.dpad.left.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.navigateLeft) }
        }
        gamepad.dpad.right.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.navigateRight) }
        }
        
        // Left Thumbstick Analog Navigation with Deadzone and Cooldown
        gamepad.leftThumbstick.valueChangedHandler = { [weak self] _, xVal, yVal in
            guard let self = self else { return }
            let now = ProcessInfo.processInfo.systemUptime
            guard now - self.lastNavTime > self.navCooldown else { return }
            
            if xVal > self.stickDeadzone {
                self.lastNavTime = now
                self.triggerAction(.navigateRight)
            } else if xVal < -self.stickDeadzone {
                self.lastNavTime = now
                self.triggerAction(.navigateLeft)
            } else if yVal > self.stickDeadzone {
                self.lastNavTime = now
                self.triggerAction(.navigateUp)
            } else if yVal < -self.stickDeadzone {
                self.lastNavTime = now
                self.triggerAction(.navigateDown)
            }
        }
        
        // Face Buttons
        gamepad.buttonA.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.confirm) }
        }
        gamepad.buttonB.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.cancel) }
        }
        gamepad.buttonX.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.secondaryAction) }
        }
        gamepad.buttonY.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.contextAction) }
        }
        
        // Shoulders: Tab Switching
        gamepad.leftShoulder.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.tabPrevious) }
        }
        gamepad.rightShoulder.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.tabNext) }
        }
        
        // Menu / Options
        gamepad.buttonMenu.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed { self?.triggerAction(.menu) }
        }
    }
    
    private func triggerAction(_ action: GamepadAction) {
        DispatchQueue.main.async { [weak self] in
            self?.onAction?(action)
        }
    }
}
