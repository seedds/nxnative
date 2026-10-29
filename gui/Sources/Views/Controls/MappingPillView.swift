import SwiftUI

public struct MappingPillView: View {
    public let button: SwitchButton
    public let mappedLabel: String
    public let isListening: Bool
    public let isLeftSide: Bool
    public let onStartListening: () -> Void
    public let onSelectMapping: (String) -> Void
    public let onReset: () -> Void

    public init(
        button: SwitchButton,
        mappedLabel: String,
        isListening: Bool,
        isLeftSide: Bool = true,
        onStartListening: @escaping () -> Void,
        onSelectMapping: @escaping (String) -> Void,
        onReset: @escaping () -> Void
    ) {
        self.button = button
        self.mappedLabel = mappedLabel
        self.isListening = isListening
        self.isLeftSide = isLeftSide
        self.onStartListening = onStartListening
        self.onSelectMapping = onSelectMapping
        self.onReset = onReset
    }

    private let availableElements: [(id: String, name: String)] = [
        ("buttonA", "A / Cross (Bottom)"),
        ("buttonB", "B / Circle (Right)"),
        ("buttonX", "X / Square (Left)"),
        ("buttonY", "Y / Triangle (Top)"),
        ("leftShoulder", "L / LB / L1"),
        ("rightShoulder", "R / RB / R1"),
        ("leftTrigger", "ZL / LT / L2"),
        ("rightTrigger", "ZR / RT / R2"),
        ("leftThumbstickButton", "L3 (Press Stick)"),
        ("rightThumbstickButton", "R3 (Press Stick)"),
        ("dpad.up", "D-Pad Up"),
        ("dpad.down", "D-Pad Down"),
        ("dpad.left", "D-Pad Left"),
        ("dpad.right", "D-Pad Right"),
        ("buttonMenu", "+ Plus / Start"),
        ("buttonOptions", "− Minus / Select"),
        ("buttonHome", "Home / Guide")
    ]

    public var body: some View {
        HStack(spacing: 8) {
            if !isLeftSide {
                switchButtonBadge
            }

            // Interactive Clickable / Listening Card
            Button(action: onStartListening) {
                HStack(spacing: 6) {
                    if isListening {
                        ProgressView()
                            .controlSize(.mini)
                        Text(L10n.t("controls.press_button"))
                            .font(.system(size: 11, weight: .bold, design: .rounded))
                            .foregroundColor(.white)
                    } else {
                        Text(mappedLabel)
                            .font(.system(size: 11, weight: .semibold, design: .rounded))
                            .foregroundColor(.white)
                            .lineLimit(1)
                    }

                    Menu {
                        ForEach(availableElements, id: \.id) { elem in
                            Button(elem.name) {
                                onSelectMapping(elem.id)
                            }
                        }
                        Divider()
                        Button(L10n.t("common.reset_default"), action: onReset)
                    } label: {
                        Image(systemName: "chevron.down")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundColor(.white)
                    }
                    .menuStyle(.borderlessButton)
                    .menuIndicator(.hidden)
                    .fixedSize()
                }
                .padding(.horizontal, 9)
                .padding(.vertical, 5)
                .contentShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .nxGlassInteractive(
                    in: RoundedRectangle(cornerRadius: 8, style: .continuous),
                    tint: isListening ? NXTheme.primary.opacity(0.68) : nil,
                    isSelected: isListening,
                    fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.85),
                    fallbackSelectedFill: NXTheme.primary.opacity(0.35)
                )
            }
            .buttonStyle(NXPressableButtonStyle())
            .nxNoFocusRing()

            if isLeftSide {
                switchButtonBadge
            }
        }
    }

    private var switchButtonBadge: some View {
        Text(button.displayName)
            .font(.system(size: 11, weight: .bold, design: .rounded))
            .foregroundColor(.white)
            .lineLimit(1)
            .frame(minWidth: 70, alignment: isLeftSide ? .leading : .trailing)
    }
}
