import SwiftUI
import AppKit

public struct SimpleKeyboardMappingView: View {
    @Binding public var profile: ControllerProfile
    @State private var listeningButton: SwitchButton? = nil
    @State private var listeningDirection: String? = nil

    public init(profile: Binding<ControllerProfile>) {
        self._profile = profile
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            // MARK: - Presets Bar
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
            .background(NXTheme.glassSurfaceElevated.opacity(0.6))
            .cornerRadius(10)

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
        }
        .background(
            KeyboardEventBridge { keyCode, keyName in
                handleCapturedKey(keyCode: keyCode, keyName: keyName)
            }
        )
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
            content()
        }
        .padding(14)
        .background(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(NXTheme.glassSurfaceElevated.opacity(0.7))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }

    private func buttonRow(btn: SwitchButton, defaultKey: String) -> some View {
        let isListening = listeningButton == btn
        let boundCode = profile.keyboardBindings.first(where: { $0.value == btn })?.key
        let keyText = boundCode.map { keyName(for: $0) } ?? defaultKey

        return HStack {
            Text(btn.displayName)
                .font(.system(size: 12, weight: .medium, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            Spacer()
            keyButton(text: isListening ? "Press Key..." : keyText, isListening: isListening) {
                listeningDirection = nil
                listeningButton = btn
                SoundEffectService.shared.play(.focus)
            }
        }
    }

    private func movementRow(direction: String, label: String, defaultKey: String) -> some View {
        let isListening = listeningDirection == direction
        let boundCode = profile.keyboardMovement[direction]
        let keyText = boundCode.map { keyName(for: $0) } ?? defaultKey

        return HStack {
            Text(label)
                .font(.system(size: 12, weight: .medium, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            Spacer()
            keyButton(text: isListening ? "Press Key..." : keyText, isListening: isListening) {
                listeningButton = nil
                listeningDirection = direction
                SoundEffectService.shared.play(.focus)
            }
        }
    }

    private func keyButton(text: String, isListening: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(text)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(isListening ? NXTheme.secondary : NXTheme.textPrimary)
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .fill(isListening ? NXTheme.primary.opacity(0.3) : Color(white: 0.16))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .strokeBorder(isListening ? NXTheme.secondary : Color.white.opacity(0.2), lineWidth: isListening ? 1.5 : 0.8)
                )
        }
        .buttonStyle(.plain)
    }

    private func handleCapturedKey(keyCode: UInt16, keyName: String) {
        if let btn = listeningButton {
            // Remove previous binding
            profile.keyboardBindings = profile.keyboardBindings.filter { $0.value != btn && $0.key != keyCode }
            profile.keyboardBindings[keyCode] = btn
            listeningButton = nil
            SoundEffectService.shared.play(.click)
            InputProfileManager.shared.updateProfile(profile)
        } else if let dir = listeningDirection {
            profile.keyboardMovement[dir] = keyCode
            listeningDirection = nil
            SoundEffectService.shared.play(.click)
            InputProfileManager.shared.updateProfile(profile)
        }
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

    private func keyName(for keyCode: UInt16) -> String {
        switch keyCode {
        case 0: return "A"
        case 1: return "S"
        case 2: return "D"
        case 3: return "F"
        case 5: return "G"
        case 6: return "Z"
        case 7: return "X"
        case 8: return "C"
        case 9: return "V"
        case 12: return "Q"
        case 13: return "W"
        case 14: return "E"
        case 32: return "U"
        case 34: return "I"
        case 36: return "Return"
        case 38: return "J"
        case 40: return "K"
        case 48: return "Tab"
        case 49: return "Space"
        case 53: return "Esc"
        case 123: return "←"
        case 124: return "→"
        case 125: return "↓"
        case 126: return "↑"
        default: return "Key #\(keyCode)"
        }
    }
}

// Helper to capture physical key presses
private struct KeyboardEventBridge: NSViewRepresentable {
    let onKeyDown: (UInt16, String) -> Void

    func makeNSView(context: Context) -> BridgeNSView {
        let view = BridgeNSView()
        view.onKeyDown = onKeyDown
        return view
    }

    func updateNSView(_ nsView: BridgeNSView, context: Context) {
        nsView.onKeyDown = onKeyDown
    }

    class BridgeNSView: NSView {
        var onKeyDown: ((UInt16, String) -> Void)?
        private var monitor: Any?

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if window != nil && monitor == nil {
                monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
                    self?.onKeyDown?(event.keyCode, event.charactersIgnoringModifiers ?? "")
                    return event
                }
            } else if window == nil, let m = monitor {
                NSEvent.removeMonitor(m)
                monitor = nil
            }
        }

        deinit {
            if let m = monitor {
                NSEvent.removeMonitor(m)
            }
        }
    }
}
