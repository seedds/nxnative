import SwiftUI
import AppKit

public struct SimpleKeyboardMappingView: View {
    @Binding public var profile: ControllerProfile
    @ObservedObject private var profileManager = InputProfileManager.shared

    public init(profile: Binding<ControllerProfile>) {
        self._profile = profile
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            // MARK: - Listening Banner
            if profileManager.listeningTarget != nil {
                HStack(spacing: 8) {
                    Image(systemName: "cursorarrow.and.square.on.square.dashed")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundColor(.white)
                    Text(L10n.t("controls.press_key_or_mouse_banner"))
                        .font(.system(size: 12, weight: .semibold, design: .rounded))
                        .foregroundColor(.white)
                    Spacer()
                    Button(action: {
                        profileManager.cancelListening()
                    }) {
                        Text(L10n.t("controls.press_esc_cancel"))
                            .font(.system(size: 10, weight: .bold, design: .monospaced))
                            .padding(.horizontal, 8)
                            .padding(.vertical, 3.5)
                            .background(Capsule().fill(Color.white.opacity(0.20)))
                            .foregroundColor(.white)
                    }
                    .buttonStyle(PlainButtonStyle())
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                .background(
                    Capsule(style: .continuous)
                        .fill(
                            LinearGradient(
                                colors: [NXTheme.primary, NXTheme.secondary],
                                startPoint: .leading,
                                endPoint: .trailing
                            )
                        )
                        .shadow(color: NXTheme.primary.opacity(0.50), radius: 8, x: 0, y: 2)
                )
                .transition(.scale.combined(with: .opacity))
            }

            // MARK: - Presets Bar
            NXGlassContainer(spacing: 8) {
                HStack(spacing: 10) {
                    Text(L10n.t("controls.keyboard_presets"))
                        .font(.system(size: 12, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)

                    Button(L10n.t("controls.preset_wasd")) {
                        applyDefaultPreset()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.small)

                    Button(L10n.t("controls.preset_arrows")) {
                        applyArrowsPreset()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.small)

                    Spacer()
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                .nxGlassCard(cornerRadius: 10, isNested: true)
            }

            // MARK: - Mapping Sections Grid
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 16), GridItem(.flexible(), spacing: 16)], spacing: 16) {
                // Section 1: Movement (Left Stick)
                mappingGroupCard(title: "Movement (Left Stick)", icon: "arrow.up.and.down.and.arrow.left.and.right") {
                    VStack(spacing: 8) {
                        movementRow(direction: "up", label: "Move Up", defaultKey: "W")
                        movementRow(direction: "left", label: "Move Left", defaultKey: "A")
                        movementRow(direction: "down", label: "Move Down", defaultKey: "S")
                        movementRow(direction: "right", label: "Move Right", defaultKey: "D")
                    }
                }

                // Section 2: Face Buttons
                mappingGroupCard(title: "Face Buttons (ABXY)", icon: "circle.grid.2x2.fill") {
                    VStack(spacing: 8) {
                        buttonRow(btn: .a, defaultKey: "J")
                        buttonRow(btn: .b, defaultKey: "K")
                        buttonRow(btn: .x, defaultKey: "U")
                        buttonRow(btn: .y, defaultKey: "I")
                    }
                }

                // Section 3: Shoulders & Triggers
                mappingGroupCard(title: "Bumpers & Triggers", icon: "shield.fill") {
                    VStack(spacing: 8) {
                        buttonRow(btn: .l, defaultKey: "Q")
                        buttonRow(btn: .r, defaultKey: "E")
                        buttonRow(btn: .zl, defaultKey: "Z")
                        buttonRow(btn: .zr, defaultKey: "C")
                    }
                }

                // Section 4: System & Stick Clicks
                mappingGroupCard(title: "System & Stick Clicks", icon: "slider.horizontal.3") {
                    VStack(spacing: 8) {
                        buttonRow(btn: .plus, defaultKey: "Return")
                        buttonRow(btn: .minus, defaultKey: "Tab")
                        buttonRow(btn: .leftStickClick, defaultKey: "F")
                        buttonRow(btn: .rightStickClick, defaultKey: "G")
                    }
                }
            }

            // MARK: - Section 5: Mouse as Right Stick
            MouseLookSettingsView(profile: $profile)
        }
        .background(InputCaptureBridge())
        .onReceive(profileManager.$profiles) { _ in
            profile = profileManager.activeProfile
        }
    }

    private func mappingGroupCard<Content: View>(
        title: String,
        icon: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 6) {
                Image(systemName: icon)
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(NXTheme.secondary)
                Text(title)
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Spacer()
            }
            NXGlassContainer(spacing: 6) {
                content()
            }
        }
        .padding(14)
        .nxGlassCard(cornerRadius: 12, isNested: true)
    }

    private func buttonRow(btn: SwitchButton, defaultKey: String) -> some View {
        let isListening = profileManager.listeningTarget == .switchButton(btn)
        let boundCode = profile.keyboardBindings.first(where: { $0.value == btn })?.key
        let keyText = boundCode.map { KeyNames.name(for: $0) } ?? defaultKey

        return HStack {
            Text(btn.displayName)
                .font(.system(size: 12, weight: .medium, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            Spacer()
            keyButton(text: isListening ? L10n.t("controls.press_key_or_mouse") : keyText, isListening: isListening) {
                if isListening {
                    profileManager.cancelListening()
                } else {
                    profileManager.startListening(for: .switchButton(btn))
                    SoundEffectService.shared.play(.focus)
                }
            }
        }
    }

    private func movementRow(direction: String, label: String, defaultKey: String) -> some View {
        let isListening = profileManager.listeningTarget == .movement(direction)
        let boundCode = profile.keyboardMovement[direction]
        let keyText = boundCode.map { KeyNames.name(for: $0) } ?? defaultKey

        return HStack {
            Text(label)
                .font(.system(size: 12, weight: .medium, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            Spacer()
            keyButton(text: isListening ? L10n.t("controls.press_key_or_mouse") : keyText, isListening: isListening) {
                if isListening {
                    profileManager.cancelListening()
                } else {
                    profileManager.startListening(for: .movement(direction))
                    SoundEffectService.shared.play(.focus)
                }
            }
        }
    }

    private func keyButton(text: String, isListening: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(text)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(isListening ? .white : NXTheme.textPrimary)
                .padding(.horizontal, 10)
                .padding(.vertical, 4.5)
                .contentShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                .nxGlassInteractive(
                    in: RoundedRectangle(cornerRadius: 6, style: .continuous),
                    tint: isListening ? NXTheme.primary.opacity(0.68) : nil,
                    isSelected: isListening,
                    fallbackFill: Color(white: 0.16),
                    fallbackSelectedFill: NXTheme.primary.opacity(0.32)
                )
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
    }

    private func applyDefaultPreset() {
        SoundEffectService.shared.play(.click)
        profile.keyboardMovement = ["up": 13, "left": 0, "down": 1, "right": 2] // WASD
        for btn in SwitchButton.allCases {
            profile.keyboardBindings[btn.defaultKeyCode] = btn
        }
        InputProfileManager.shared.updateProfile(profile)
    }

    private func applyArrowsPreset() {
        SoundEffectService.shared.play(.click)
        profile.keyboardMovement = ["up": 126, "left": 123, "down": 125, "right": 124] // Arrow keys
        profile.keyboardBindings[6] = .b  // Z -> B
        profile.keyboardBindings[7] = .a  // X -> A
        profile.keyboardBindings[8] = .y  // C -> Y
        profile.keyboardBindings[9] = .x  // V -> X
        InputProfileManager.shared.updateProfile(profile)
    }

}
