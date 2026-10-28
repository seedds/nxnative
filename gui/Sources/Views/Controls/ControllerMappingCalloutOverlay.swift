import SwiftUI

public struct ControllerMappingCalloutOverlay: View {
    @Binding public var profile: ControllerProfile
    public let pressedButtons: Set<SwitchButton>
    @State private var listeningButton: SwitchButton? = nil

    public init(
        profile: Binding<ControllerProfile>,
        pressedButtons: Set<SwitchButton> = []
    ) {
        self._profile = profile
        self.pressedButtons = pressedButtons
    }

    private let leftButtons: [SwitchButton] = [
        .zl, .l, .minus, .capture, .leftStickClick, .dpadUp, .dpadLeft, .dpadDown, .dpadRight
    ]

    private let rightButtons: [SwitchButton] = [
        .zr, .r, .plus, .home, .x, .y, .a, .b, .rightStickClick
    ]

    public var body: some View {
        HStack(alignment: .center, spacing: 18) {
            // MARK: - Left Callout Column
            VStack(alignment: .trailing, spacing: 10) {
                ForEach(leftButtons) { btn in
                    let mapped = profile.gamepadBindings.first(where: { $0.value == btn })?.key ?? "None"
                    MappingPillView(
                        button: btn,
                        mappedLabel: formatElementName(mapped),
                        isListening: listeningButton == btn,
                        isLeftSide: true,
                        onStartListening: {
                            startListening(for: btn)
                        },
                        onSelectMapping: { elem in
                            assignMapping(element: elem, to: btn)
                        },
                        onReset: {
                            assignMapping(element: btn.defaultGamepadElement, to: btn)
                        }
                    )
                }
            }
            .frame(width: 220)

            // MARK: - Center Controller Vector & Connecting Canvas
            VStack(spacing: 12) {
                // Quick Layout Switcher (Enhancement 2)
                HStack(spacing: 8) {
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        withAnimation(.easeInOut(duration: 0.18)) {
                            profile.swapFaceButtons()
                            InputProfileManager.shared.updateProfile(profile)
                        }
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "arrow.left.arrow.right")
                                .font(.system(size: 11, weight: .bold))
                            Text(L10n.t("controls.swap_ab_xy"))
                                .font(.system(size: 11, weight: .bold, design: .rounded))
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(NXTheme.glassSurfaceElevated)
                        .foregroundColor(.white)
                        .clipShape(Capsule())
                        .overlay(Capsule().stroke(NXTheme.specularRim, lineWidth: 0.8))
                    }
                    .buttonStyle(.plain)
                    .help(L10n.t("controls.swap_ab_xy_help"))
                }

                // Vector Controller View
                SwitchProControllerVectorView(
                    pressedButtons: pressedButtons,
                    selectedButton: listeningButton,
                    onSelectButton: { btn in
                        startListening(for: btn)
                    }
                )
                .frame(minWidth: 320, idealWidth: 380, maxWidth: 440, minHeight: 220, idealHeight: 260)
            }
            .frame(maxWidth: .infinity)

            // MARK: - Right Callout Column
            VStack(alignment: .leading, spacing: 10) {
                ForEach(rightButtons) { btn in
                    let mapped = profile.gamepadBindings.first(where: { $0.value == btn })?.key ?? "None"
                    MappingPillView(
                        button: btn,
                        mappedLabel: formatElementName(mapped),
                        isListening: listeningButton == btn,
                        isLeftSide: false,
                        onStartListening: {
                            startListening(for: btn)
                        },
                        onSelectMapping: { elem in
                            assignMapping(element: elem, to: btn)
                        },
                        onReset: {
                            assignMapping(element: btn.defaultGamepadElement, to: btn)
                        }
                    )
                }
            }
            .frame(width: 220)
        }
        .padding(.vertical, 8)
    }

    private func startListening(for btn: SwitchButton) {
        SoundEffectService.shared.play(.focus)
        listeningButton = btn

        // Temporary listener on GameController
        GameControllerService.shared.onAction = { [self] action in
            if let targetBtn = self.listeningButton {
                let mappedElem = self.elementName(for: action)
                DispatchQueue.main.async {
                    self.assignMapping(element: mappedElem, to: targetBtn)
                    self.listeningButton = nil
                    SoundEffectService.shared.play(.click)
                }
            }
        }
    }

    private func assignMapping(element: String, to btn: SwitchButton) {
        // Remove existing mapping for this element to prevent conflicts
        profile.gamepadBindings.removeValue(forKey: element)
        profile.gamepadBindings[element] = btn
        InputProfileManager.shared.updateProfile(profile)
    }

    private func elementName(for action: GamepadAction) -> String {
        switch action {
        case .confirm: return "buttonA"
        case .cancel: return "buttonB"
        case .secondaryAction: return "buttonX"
        case .contextAction: return "buttonY"
        case .tabPrevious: return "leftShoulder"
        case .tabNext: return "rightShoulder"
        case .navigateUp: return "dpad.up"
        case .navigateDown: return "dpad.down"
        case .navigateLeft: return "dpad.left"
        case .navigateRight: return "dpad.right"
        case .menu: return "buttonMenu"
        case .options: return "buttonOptions"
        }
    }

    private func formatElementName(_ element: String) -> String {
        switch element {
        case "buttonA": return "Button A / ✕"
        case "buttonB": return "Button B / ○"
        case "buttonX": return "Button X / □"
        case "buttonY": return "Button Y / △"
        case "leftShoulder": return "L Bumper (LB)"
        case "rightShoulder": return "R Bumper (RB)"
        case "leftTrigger": return "ZL Trigger (LT)"
        case "rightTrigger": return "ZR Trigger (RT)"
        case "leftThumbstickButton": return "Left Stick Click (L3)"
        case "rightThumbstickButton": return "Right Stick Click (R3)"
        case "dpad.up": return "D-Pad Up"
        case "dpad.down": return "D-Pad Down"
        case "dpad.left": return "D-Pad Left"
        case "dpad.right": return "D-Pad Right"
        case "buttonMenu": return "+ Plus / Start"
        case "buttonOptions": return "− Minus / Select"
        case "buttonHome": return "Home / Guide"
        default: return element
        }
    }
}
