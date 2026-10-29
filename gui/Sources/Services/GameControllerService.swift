import Foundation
import GameController

public final class GameControllerService: ObservableObject {
    public static let shared = GameControllerService()
    
    @Published public var isGamepadConnected: Bool = false
    @Published public var activeControllerName: String = "No Gamepad Connected"
    @Published public var activeBrand: GamepadBrand = .generic
    @Published public var activeRunningGameApp: NSRunningApplication? = nil
    
    public var onAction: ((GamepadAction) -> Void)?
    public var onRawElementPressed: ((String) -> Void)?
    
    private var lastNavTime: TimeInterval = 0
    private let navCooldown: TimeInterval = 0.20
    private let stickDeadzone: Float = 0.35
    private var homeButtonHoldTimer: Timer?
    
    private init() {
        GCController.shouldMonitorBackgroundEvents = true
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
            if pressed {
                self?.triggerRawElement("dpad.up")
                self?.triggerAction(.navigateUp)
            }
        }
        gamepad.dpad.down.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("dpad.down")
                self?.triggerAction(.navigateDown)
            }
        }
        gamepad.dpad.left.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("dpad.left")
                self?.triggerAction(.navigateLeft)
            }
        }
        gamepad.dpad.right.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("dpad.right")
                self?.triggerAction(.navigateRight)
            }
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
            if pressed {
                self?.triggerRawElement("buttonA")
                self?.triggerAction(.confirm)
            }
        }
        gamepad.buttonB.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("buttonB")
                self?.triggerAction(.cancel)
            }
        }
        gamepad.buttonX.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("buttonX")
                self?.triggerAction(.secondaryAction)
            }
        }
        gamepad.buttonY.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("buttonY")
                self?.triggerAction(.contextAction)
            }
        }
        
        // Shoulders: Tab Switching / Bumpers
        gamepad.leftShoulder.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("leftShoulder")
                self?.triggerAction(.tabPrevious)
            }
        }
        gamepad.rightShoulder.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("rightShoulder")
                self?.triggerAction(.tabNext)
            }
        }
        
        // Triggers (ZL / ZR)
        gamepad.leftTrigger.valueChangedHandler = { [weak self] _, val, pressed in
            if pressed || val > 0.35 {
                self?.triggerRawElement("leftTrigger")
            }
        }
        gamepad.rightTrigger.valueChangedHandler = { [weak self] _, val, pressed in
            if pressed || val > 0.35 {
                self?.triggerRawElement("rightTrigger")
            }
        }
        
        // Thumbstick Buttons (L3 / R3)
        gamepad.leftThumbstickButton?.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("leftThumbstickButton")
            }
        }
        gamepad.rightThumbstickButton?.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("rightThumbstickButton")
            }
        }
        
        // Menu / Plus
        gamepad.buttonMenu.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("buttonMenu")
                self?.triggerAction(.menu)
            }
        }
        
        // Options / Minus / Share / View
        gamepad.buttonOptions?.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("buttonOptions")
                self?.triggerAction(.options)
            }
        }
        
        // Home Button (Xbox Logo / Guide): Pressed & held for 3 seconds quits game & brings NxNative to front
        gamepad.buttonHome?.pressedChangedHandler = { [weak self] _, _, pressed in
            if pressed {
                self?.triggerRawElement("buttonHome")
                self?.startHomeHoldTimer()
            } else {
                self?.cancelHomeHoldTimer()
            }
        }
    }
    
    private func triggerRawElement(_ element: String) {
        DispatchQueue.main.async { [weak self] in
            guard NSApplication.shared.isActive else { return }
            self?.onRawElementPressed?(element)
        }
    }
    
    private func startHomeHoldTimer() {
        homeButtonHoldTimer?.invalidate()
        homeButtonHoldTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: false) { [weak self] _ in
            self?.handleHomeHold3Seconds()
        }
    }
    
    private func cancelHomeHoldTimer() {
        homeButtonHoldTimer?.invalidate()
        homeButtonHoldTimer = nil
    }
    
    private func handleHomeHold3Seconds() {
        DispatchQueue.main.async {
            // 1. Terminate tracked running game app
            if let app = self.activeRunningGameApp, !app.isTerminated {
                app.terminate()
            }
            self.activeRunningGameApp = nil
            
            // 2. Terminate running game instances (nxnative / dmhost)
            for proc in ["nxnative", "dmhost"] {
                let killProc = Process()
                killProc.executableURL = URL(fileURLWithPath: "/usr/bin/pkill")
                killProc.arguments = ["-TERM", proc]
                try? killProc.run()
            }
            
            // 3. Bring NxNative Launcher to front of other screens (and restore from Menu Bar if minimized)
            MenuBarManager.shared.restoreMainWindow()
            SoundEffectService.shared.play(.error)
        }
    }
    
    private func triggerAction(_ action: GamepadAction) {
        DispatchQueue.main.async { [weak self] in
            // Guard against intercepting gamepad events while user is playing another game in foreground
            guard NSApplication.shared.isActive else { return }
            guard !MenuBarManager.isMinimized else { return }
            self?.onAction?(action)
        }
    }
}
