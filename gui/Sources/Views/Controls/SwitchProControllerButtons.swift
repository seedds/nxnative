import SwiftUI

// MARK: - Subcomponents & Controls for SwitchProControllerVectorView
extension SwitchProControllerVectorView {

    // MARK: - Top Shoulder Bumpers (L, R)
    func topShouldersView(width w: CGFloat, height h: CGFloat) -> some View {
        ZStack {
            bumperButton(title: "L", btn: .l, x: w * 0.243, y: h * 0.164, w: w * 0.13, h: h * 0.065)
            bumperButton(title: "R", btn: .r, x: w * 0.757, y: h * 0.164, w: w * 0.13, h: h * 0.065)
        }
    }

    func bumperButton(
        title: String,
        btn: SwitchButton,
        x: CGFloat,
        y: CGFloat,
        w: CGFloat,
        h: CGFloat
    ) -> some View {
        let isPressed = pressedButtons.contains(btn)
        let isSel = selectedButton == btn
        return Button(action: { onSelectButton(btn) }) {
            ZStack {
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(isPressed ? NXTheme.secondary : (isSel ? NXTheme.primary : Color(white: 0.18)))
                Text(title)
                    .font(.system(size: 9, weight: .bold, design: .rounded))
                    .foregroundColor(.white)
            }
            .frame(width: w, height: h)
            .overlay(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .strokeBorder(isSel ? Color.white : Color.white.opacity(0.3), lineWidth: isSel ? 2 : 0.8)
            )
            .shadow(color: isSel ? NXTheme.primary.opacity(0.8) : Color.clear, radius: 4)
        }
        .buttonStyle(NXPressableButtonStyle())
        .position(x: x, y: y)
    }

    // MARK: - Center System Buttons (+, −, Capture, Home)
    func centerButtonsView(width w: CGFloat, height h: CGFloat) -> some View {
        ZStack {
            centerRoundButton(btn: .minus, symbol: "−", x: w * 0.353, y: h * 0.323, size: w * 0.045)
            centerRoundButton(btn: .plus, symbol: "+", x: w * 0.593, y: h * 0.323, size: w * 0.045)
            centerRoundButton(btn: .capture, symbol: "□", x: w * 0.404, y: h * 0.412, size: w * 0.04)
            centerRoundButton(btn: .home, symbol: "⌂", x: w * 0.542, y: h * 0.412, size: w * 0.04)
        }
    }

    func centerRoundButton(
        btn: SwitchButton,
        symbol: String,
        x: CGFloat,
        y: CGFloat,
        size: CGFloat
    ) -> some View {
        let isPressed = pressedButtons.contains(btn)
        let isSel = selectedButton == btn
        return Button(action: { onSelectButton(btn) }) {
            ZStack {
                Circle()
                    .fill(isPressed ? NXTheme.secondary : (isSel ? NXTheme.primary : Color(white: 0.22)))
                Text(symbol)
                    .font(.system(size: 9, weight: .bold))
                    .foregroundColor(.white)
            }
            .frame(width: size, height: size)
            .overlay(
                Circle().stroke(isSel ? Color.white : Color.white.opacity(0.35), lineWidth: isSel ? 2 : 0.8)
            )
            .shadow(color: isSel ? NXTheme.primary.opacity(0.8) : Color.clear, radius: 4)
        }
        .buttonStyle(NXPressableButtonStyle())
        .position(x: x, y: y)
    }

    // MARK: - Analog Sticks
    func stickView(
        centerX: CGFloat,
        centerY: CGFloat,
        radius: CGFloat,
        isLeft: Bool,
        isPressed: Bool,
        isSelected: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            ZStack {
                Circle()
                    .fill(Color(white: 0.08))
                    .frame(width: radius * 2, height: radius * 2)
                    .overlay(Circle().stroke(Color.white.opacity(0.15), lineWidth: 1))
                Circle()
                    .fill(
                        RadialGradient(
                            colors: [
                                isPressed ? NXTheme.secondary : (isSelected ? NXTheme.primary : Color(white: 0.26)),
                                Color(white: 0.14)
                            ],
                            center: .center,
                            startRadius: 2,
                            endRadius: radius * 0.8
                        )
                    )
                    .frame(width: radius * 1.5, height: radius * 1.5)
                    .overlay(
                        Circle().stroke(isSelected ? Color.white : Color.white.opacity(0.3), lineWidth: isSelected ? 2 : 0.8)
                    )
                Text(isLeft ? "L3" : "R3")
                    .font(.system(size: 8, weight: .bold, design: .monospaced))
                    .foregroundColor(Color.white.opacity(0.85))
            }
        }
        .buttonStyle(NXPressableButtonStyle())
        .position(x: centerX, y: centerY)
    }

    // MARK: - Directional Pad (D-Pad)
    func dpadView(centerX cx: CGFloat, centerY cy: CGFloat, size: CGFloat) -> some View {
        let arm = size * 0.36
        return ZStack {
            dpadDirectionButton(btn: .dpadUp, label: "▲", x: cx, y: cy - arm * 0.78, w: arm, h: arm * 0.78)
            dpadDirectionButton(btn: .dpadDown, label: "▼", x: cx, y: cy + arm * 0.78, w: arm, h: arm * 0.78)
            dpadDirectionButton(btn: .dpadLeft, label: "◀", x: cx - arm * 0.78, y: cy, w: arm * 0.78, h: arm)
            dpadDirectionButton(btn: .dpadRight, label: "▶", x: cx + arm * 0.78, y: cy, w: arm * 0.78, h: arm)
            Circle()
                .fill(Color(white: 0.16))
                .frame(width: arm * 0.55, height: arm * 0.55)
                .position(x: cx, y: cy)
        }
    }

    func dpadDirectionButton(
        btn: SwitchButton,
        label: String,
        x: CGFloat,
        y: CGFloat,
        w: CGFloat,
        h: CGFloat
    ) -> some View {
        let isPressed = pressedButtons.contains(btn)
        let isSel = selectedButton == btn
        return Button(action: { onSelectButton(btn) }) {
            ZStack {
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .fill(isPressed ? NXTheme.secondary : (isSel ? NXTheme.primary : Color(white: 0.20)))
                Text(label)
                    .font(.system(size: 8, weight: .bold))
                    .foregroundColor(.white)
            }
            .frame(width: w, height: h)
            .overlay(
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .stroke(isSel ? Color.white : Color.white.opacity(0.25), lineWidth: isSel ? 1.5 : 0.5)
            )
            .shadow(color: isSel ? NXTheme.primary.opacity(0.7) : Color.clear, radius: 3)
        }
        .buttonStyle(NXPressableButtonStyle())
        .position(x: x, y: y)
    }

    // MARK: - Face Buttons ABXY (Nintendo Switch Pro Layout)
    func faceButtonsView(centerX cx: CGFloat, centerY cy: CGFloat, size: CGFloat) -> some View {
        let arm = size * 0.48
        return ZStack {
            faceButton(btn: .x, label: "X", x: cx, y: cy - arm, color: Color(red: 0.0, green: 0.47, blue: 0.84))
            faceButton(btn: .a, label: "A", x: cx + arm, y: cy, color: Color(red: 0.90, green: 0.0, blue: 0.07))
            faceButton(btn: .b, label: "B", x: cx, y: cy + arm, color: Color(red: 0.91, green: 0.07, blue: 0.14))
            faceButton(btn: .y, label: "Y", x: cx - arm, y: cy, color: Color(red: 0.10, green: 0.65, blue: 0.25))
        }
    }

    func faceButton(
        btn: SwitchButton,
        label: String,
        x: CGFloat,
        y: CGFloat,
        color: Color
    ) -> some View {
        let isPressed = pressedButtons.contains(btn)
        let isSel = selectedButton == btn
        return Button(action: { onSelectButton(btn) }) {
            ZStack {
                Circle()
                    .fill(isPressed ? NXTheme.secondary : (isSel ? NXTheme.primary : Color(white: 0.22)))
                Text(label)
                    .font(.system(size: 10, weight: .black, design: .rounded))
                    .foregroundColor(.white)
            }
            .frame(width: 20, height: 20)
            .overlay(
                Circle().stroke(isSel ? Color.white : color.opacity(0.8), lineWidth: isSel ? 2 : 1)
            )
            .shadow(color: isSel ? color.opacity(0.85) : Color.clear, radius: 4)
        }
        .buttonStyle(NXPressableButtonStyle())
        .position(x: x, y: y)
    }
}
