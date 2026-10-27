import SwiftUI
import GameController

public struct AnalogDeadzoneTestView: View {
    @Binding public var deadzone: Double
    @State private var leftX: Float = 0
    @State private var leftY: Float = 0
    @State private var rightX: Float = 0
    @State private var rightY: Float = 0
    @State private var timer: Timer? = nil

    public init(deadzone: Binding<Double>) {
        self._deadzone = deadzone
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label("Analog Stick Calibration & Drift Test", systemImage: "scope")
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)

                Spacer()

                HStack(spacing: 8) {
                    Text("Deadzone: \(Int(deadzone * 100))%")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(NXTheme.secondary)

                    Slider(value: $deadzone, in: 0.05...0.30, step: 0.01)
                        .frame(width: 120)
                        .tint(NXTheme.primary)
                }
            }

            HStack(spacing: 32) {
                // Left Stick Target
                stickRadarView(label: "Left Stick (LS)", x: leftX, y: leftY)

                // Right Stick Target
                stickRadarView(label: "Right Stick (RS)", x: rightX, y: rightY)

                VStack(alignment: .leading, spacing: 4) {
                    Text("Live Drift Monitor:")
                        .font(.system(size: 11, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textSecondary)

                    let isDrifting = isStickDrifting(x: leftX, y: leftY) || isStickDrifting(x: rightX, y: rightY)
                    Text(isDrifting ? "Drift Detected outside Deadzone" : "Centered & Stable (Within Deadzone)")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(isDrifting ? Color(red: 0.91, green: 0.07, blue: 0.14) : Color(red: 0.13, green: 0.77, blue: 0.37))

                    Text("Adjust the deadzone slider above if your character moves on its own without touching the stick.")
                        .font(.system(size: 10))
                        .foregroundColor(NXTheme.textMuted)
                }
            }
        }
        .padding(14)
        .background(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(NXTheme.glassSurfaceElevated.opacity(0.65))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
        .onAppear {
            startPolling()
        }
        .onDisappear {
            timer?.invalidate()
            timer = nil
        }
    }

    private func stickRadarView(label: String, x: Float, y: Float) -> some View {
        VStack(spacing: 6) {
            ZStack {
                // Outer circle
                Circle()
                    .fill(Color(white: 0.09))
                    .frame(width: 76, height: 76)
                    .overlay(Circle().stroke(Color.white.opacity(0.15), lineWidth: 1))

                // Deadzone safe zone
                Circle()
                    .stroke(Color.yellow.opacity(0.4), style: StrokeStyle(lineWidth: 1, dash: [2, 2]))
                    .frame(width: CGFloat(deadzone) * 76, height: CGFloat(deadzone) * 76)

                // Axes lines
                Rectangle()
                    .fill(Color.white.opacity(0.12))
                    .frame(width: 76, height: 1)
                Rectangle()
                    .fill(Color.white.opacity(0.12))
                    .frame(width: 1, height: 76)

                // Live deflection dot
                let posX = CGFloat(x) * 34
                let posY = CGFloat(-y) * 34 // Invert Y for screen coordinates
                Circle()
                    .fill(isStickDrifting(x: x, y: y) ? NXTheme.secondary : Color.white)
                    .frame(width: 8, height: 8)
                    .offset(x: posX, y: posY)
                    .shadow(color: NXTheme.secondary.opacity(0.8), radius: 3)
            }

            Text(label)
                .font(.system(size: 10, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textSecondary)

            Text(String(format: "X: %+.2f  Y: %+.2f", x, y))
                .font(.system(size: 9, design: .monospaced))
                .foregroundColor(NXTheme.textMuted)
        }
    }

    private func isStickDrifting(x: Float, y: Float) -> Bool {
        let mag = sqrt(Double(x * x + y * y))
        return mag > deadzone
    }

    private func startPolling() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
            if let pad = GCController.controllers().first?.extendedGamepad {
                self.leftX = pad.leftThumbstick.xAxis.value
                self.leftY = pad.leftThumbstick.yAxis.value
                self.rightX = pad.rightThumbstick.xAxis.value
                self.rightY = pad.rightThumbstick.yAxis.value
            }
        }
    }
}
