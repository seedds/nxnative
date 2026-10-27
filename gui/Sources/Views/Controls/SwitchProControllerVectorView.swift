import SwiftUI
import AppKit

public struct SwitchProControllerVectorView: View {
    public let pressedButtons: Set<SwitchButton>
    public let selectedButton: SwitchButton?
    public let onSelectButton: (SwitchButton) -> Void

    private static let svgImage: NSImage? = {
        if let u = Bundle.main.url(forResource: "SwitchProController", withExtension: "svg"),
           let img = NSImage(contentsOf: u) {
            return img
        }
        let bundleResources = Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/SwitchProController.svg")
        if let img = NSImage(contentsOf: bundleResources) {
            return img
        }
        let devPath = URL(fileURLWithPath: "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/gui/Resources/SwitchProController.svg")
        if let img = NSImage(contentsOf: devPath) {
            return img
        }
        return nil
    }()

    public init(
        pressedButtons: Set<SwitchButton> = [],
        selectedButton: SwitchButton? = nil,
        onSelectButton: @escaping (SwitchButton) -> Void = { _ in }
    ) {
        self.pressedButtons = pressedButtons
        self.selectedButton = selectedButton
        self.onSelectButton = onSelectButton
    }

    public var body: some View {
        GeometryReader { geo in
            let aspect: CGFloat = 148.0 / 122.0
            let availableWidth = geo.size.width
            let availableHeight = geo.size.height
            let w = min(availableWidth, availableHeight * aspect)
            let h = w / aspect

            ZStack {
                // MARK: - Authentic Nintendo Switch Pro Controller SVG Artwork
                if let img = Self.svgImage {
                    Image(nsImage: img)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: w, height: h)
                        .shadow(color: Color.black.opacity(0.6), radius: 18, x: 0, y: 8)
                } else {
                    controllerChassisPath(width: w, height: h)
                        .fill(Color(white: 0.15))
                }

                // Top Shoulder Bumpers (L, ZL, R, ZR)
                topShouldersView(width: w, height: h)

                // Center System Buttons: Minus, Plus, Capture, Home
                centerButtonsView(width: w, height: h)

                // Left Stick (Upper-Left on Switch Pro)
                stickView(
                    centerX: w * 0.205,
                    centerY: h * 0.412,
                    radius: w * 0.075,
                    isLeft: true,
                    isPressed: pressedButtons.contains(.leftStickClick),
                    isSelected: selectedButton == .leftStickClick
                ) {
                    onSelectButton(.leftStickClick)
                }

                // Directional Pad (Lower-Left on Switch Pro)
                dpadView(centerX: w * 0.323, centerY: h * 0.575, size: w * 0.16)

                // Face Buttons ABXY (Upper-Right on Switch Pro)
                faceButtonsView(centerX: w * 0.730, centerY: h * 0.412, size: w * 0.16)

                // Right Stick (Lower-Right on Switch Pro)
                stickView(
                    centerX: w * 0.601,
                    centerY: h * 0.575,
                    radius: w * 0.075,
                    isLeft: false,
                    isPressed: pressedButtons.contains(.rightStickClick),
                    isSelected: selectedButton == .rightStickClick
                ) {
                    onSelectButton(.rightStickClick)
                }
            }
            .frame(width: w, height: h)
            .position(x: availableWidth / 2, y: availableHeight / 2)
        }
        .aspectRatio(148.0 / 122.0, contentMode: .fit)
    }

    // Fallback silhouette if SVG is unavailable
    private func controllerChassisPath(width w: CGFloat, height h: CGFloat) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: w * 0.28, y: h * 0.22))
        p.addQuadCurve(to: CGPoint(x: w * 0.72, y: h * 0.22), control: CGPoint(x: w * 0.50, y: h * 0.18))
        p.addQuadCurve(to: CGPoint(x: w * 0.88, y: h * 0.36), control: CGPoint(x: w * 0.84, y: h * 0.23))
        p.addQuadCurve(to: CGPoint(x: w * 0.80, y: h * 0.94), control: CGPoint(x: w * 0.96, y: h * 0.68))
        p.addQuadCurve(to: CGPoint(x: w * 0.64, y: h * 0.84), control: CGPoint(x: w * 0.72, y: h * 0.96))
        p.addQuadCurve(to: CGPoint(x: w * 0.36, y: h * 0.84), control: CGPoint(x: w * 0.50, y: h * 0.76))
        p.addQuadCurve(to: CGPoint(x: w * 0.20, y: h * 0.94), control: CGPoint(x: w * 0.28, y: h * 0.96))
        p.addQuadCurve(to: CGPoint(x: w * 0.12, y: h * 0.36), control: CGPoint(x: w * 0.04, y: h * 0.68))
        p.addQuadCurve(to: CGPoint(x: w * 0.28, y: h * 0.22), control: CGPoint(x: w * 0.16, y: h * 0.23))
        p.closeSubpath()
        return p
    }
}
