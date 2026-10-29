import SwiftUI

// MARK: - Single Persistent Sliding Liquid Glass Pill

/// A persistent Liquid Glass selection lens designed to be paired with
/// `.matchedGeometryEffect(id: activeSelection, in: namespace, properties: .frame, isSource: false)`
/// so selection slides smoothly across tabs, segments, filters, and menu rows like a liquid droplet.
public struct NXLiquidSlidingPill<S: InsettableShape>: View {
    private let shape: S
    private let tint: Color?
    private let fallbackFill: AnyShapeStyle

    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    public init(
        in shape: S,
        tint: Color? = nil,
        fallbackFill: some ShapeStyle = NXTheme.primaryGradient
    ) {
        self.shape = shape
        self.tint = tint
        self.fallbackFill = AnyShapeStyle(fallbackFill)
    }

    public var body: some View {
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency
        let resolvedTint = tint ?? NXTheme.primary.opacity(glassEnv.mix(frosted: 0.86, liquid: 0.68))

        // Opaque tinted gradient keeps selected text/icons crisp white (glass layers can wash them out)
        shape
            .fill(
                LinearGradient(
                    colors: [
                        resolvedTint.opacity(useGlass ? 0.92 : 1.0),
                        resolvedTint.opacity(useGlass ? 0.74 : 0.9)
                    ],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                )
            )
            .overlay(shape.strokeBorder(Color.white.opacity(0.40), lineWidth: 1.0))
            .shadow(color: resolvedTint.opacity(0.45), radius: 8, x: 0, y: 2)
    }
}

// MARK: - Liquid Glass Segmented Picker

public struct NXSegmentOption<Value: Hashable>: Identifiable {
    public let id: Value
    public let title: String
    public let icon: String?

    public init(value: Value, title: String, icon: String? = nil) {
        self.id = value
        self.title = title
        self.icon = icon
    }
}

/// Replaces flat system segmented pickers with an interactive sliding Liquid Glass capsule bar.
public struct NXLiquidSegmentedPicker<Value: Hashable>: View {
    @Binding public var selection: Value
    public let options: [NXSegmentOption<Value>]

    @Namespace private var segmentNamespace

    public init(selection: Binding<Value>, options: [NXSegmentOption<Value>]) {
        self._selection = selection
        self.options = options
    }

    public var body: some View {
        NXGlassContainer(spacing: 6) {
            HStack(spacing: 4) {
                ForEach(options) { option in
                    let isSelected = selection == option.id
                    Button(action: {
                        guard selection != option.id else { return }
                        SoundEffectService.shared.play(.click)
                        withAnimation(.spring(response: 0.34, dampingFraction: 0.76, blendDuration: 0.12)) {
                            selection = option.id
                        }
                    }) {
                        HStack(spacing: 6) {
                            if let icon = option.icon {
                                Image(systemName: icon)
                                    .font(.system(size: 12, weight: isSelected ? .bold : .semibold))
                            }
                            Text(option.title)
                                .font(.system(size: 12, weight: isSelected ? .bold : .semibold, design: .rounded))
                                .lineLimit(1)
                        }
                        .foregroundColor(.white)
                        .shadow(color: isSelected ? Color.black.opacity(0.40) : Color.clear, radius: 1, x: 0, y: 1)
                        .frame(maxWidth: .infinity)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 7)
                        .contentShape(Capsule())
                    }
                    .buttonStyle(NXPressableButtonStyle())
                    .nxNoFocusRing()
                    .matchedGeometryEffect(id: option.id, in: segmentNamespace, isSource: true)
                }
            }
            .background {
                NXLiquidSlidingPill(in: Capsule())
                    .matchedGeometryEffect(
                        id: selection,
                        in: segmentNamespace,
                        properties: .frame,
                        anchor: .center,
                        isSource: false
                    )
                    .animation(.spring(response: 0.34, dampingFraction: 0.76, blendDuration: 0.12), value: selection)
            }
            .padding(4)
            .nxGlassBar(
                in: Capsule(),
                fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.85),
                fallbackStroke: NXTheme.borderSubtle
            )
        }
    }
}

// MARK: - Liquid Glass Slider

/// Interactive Liquid Glass slider with a luminous progress track and a refractive glass droplet thumb
/// that expands and refracts the underlying bar during hover and drag interactions.
public struct NXLiquidGlassSlider: View {
    @Binding public var value: Double
    public let range: ClosedRange<Double>
    public let step: Double?
    public var accentColor: Color

    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.isEnabled) private var isControlEnabled

    @State private var isHovered: Bool = false
    @State private var isDragging: Bool = false

    public init(
        value: Binding<Double>,
        in range: ClosedRange<Double>,
        step: Double? = nil,
        accentColor: Color = NXTheme.primary
    ) {
        self._value = value
        self.range = range
        self.step = step
        self.accentColor = accentColor
    }

    private var normalizedValue: CGFloat {
        let span = range.upperBound - range.lowerBound
        guard span > 0 else { return 0 }
        let clamped = min(max(value, range.lowerBound), range.upperBound)
        return CGFloat((clamped - range.lowerBound) / span)
    }

    public var body: some View {
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency
        let thumbWidth: CGFloat = isDragging ? 28 : (isHovered ? 24 : 21)
        let thumbHeight: CGFloat = isDragging ? 19 : (isHovered ? 16 : 14)
        let barHeight: CGFloat = isDragging ? 9 : (isHovered ? 7.5 : 6.5)

        GeometryReader { geo in
            let totalWidth = max(1, geo.size.width)
            let travelWidth = max(1, totalWidth - thumbWidth)
            let thumbCenterX = (thumbWidth * 0.5) + normalizedValue * travelWidth
            let fillWidth = max(barHeight, min(totalWidth, thumbCenterX))

            ZStack(alignment: .leading) {
                // 1. Recessed Translucent Glass Track
                Capsule(style: .continuous)
                    .fill(Color.white.opacity(useGlass ? 0.14 : 0.12))
                    .frame(width: totalWidth, height: barHeight)
                    .overlay(
                        Capsule(style: .continuous)
                            .strokeBorder(Color.white.opacity(0.20), lineWidth: 0.7)
                    )

                // 2. Luminous Accent Progress Fill
                Capsule(style: .continuous)
                    .fill(
                        LinearGradient(
                            colors: [accentColor, NXTheme.secondary],
                            startPoint: .leading,
                            endPoint: .trailing
                        )
                    )
                    .frame(width: fillWidth, height: barHeight)
                    .shadow(
                        color: accentColor.opacity(isDragging ? 0.65 : 0.35),
                        radius: isDragging ? 8 : 4,
                        x: 0,
                        y: 0
                    )

                // 3. Refractive Liquid Glass Droplet Thumb
                sliderThumb(useGlass: useGlass, width: thumbWidth, height: thumbHeight)
                    .position(x: thumbCenterX, y: geo.size.height * 0.5)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
            .onHover { hovering in
                withAnimation(.spring(response: 0.24, dampingFraction: 0.75)) {
                    isHovered = hovering
                }
            }
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { gesture in
                        guard isControlEnabled else { return }
                        if !isDragging {
                            withAnimation(.spring(response: 0.22, dampingFraction: 0.72)) {
                                isDragging = true
                            }
                        }
                        updateValue(at: gesture.location.x, totalWidth: totalWidth, thumbWidth: thumbWidth)
                    }
                    .onEnded { _ in
                        withAnimation(.spring(response: 0.28, dampingFraction: 0.74)) {
                            isDragging = false
                        }
                    }
            )
        }
        .frame(height: 24)
        .opacity(isControlEnabled ? 1.0 : 0.48)
        .accessibilityElement()
        .accessibilityValue("\(Int(round(normalizedValue * 100)))%")
        .accessibilityAdjustableAction { direction in
            let delta = step ?? ((range.upperBound - range.lowerBound) / 20.0)
            switch direction {
            case .increment:
                value = min(range.upperBound, value + delta)
            case .decrement:
                value = max(range.lowerBound, value - delta)
            @unknown default:
                break
            }
        }
    }

    @ViewBuilder
    private func sliderThumb(useGlass: Bool, width: CGFloat, height: CGFloat) -> some View {
        let shape = Capsule(style: .continuous)
        if #available(macOS 26.0, *), useGlass {
            // While dragging, the thumb turns into a crystal-clear liquid droplet refracting the track underneath.
            let thumbTint = isDragging
                ? Color.white.opacity(0.16)
                : Color.white.opacity(glassEnv.mix(frosted: 0.80, liquid: 0.52))
            Color.clear
                .frame(width: width, height: height)
                .background(
                    shape.fill(isDragging ? Color.white.opacity(0.10) : Color.white.opacity(0.68))
                )
                .overlay(
                    shape.strokeBorder(
                        Color.white.opacity(isDragging ? 0.88 : 0.95),
                        lineWidth: isDragging ? 1.3 : 1.0
                    )
                )
                .glassEffect(Glass.clear.tint(thumbTint).interactive(), in: shape)
                .shadow(
                    color: isDragging ? accentColor.opacity(0.55) : Color.black.opacity(0.35),
                    radius: isDragging ? 8 : 4,
                    x: 0,
                    y: 2
                )
        } else {
            shape
                .fill(Color.white)
                .frame(width: width, height: height)
                .overlay(
                    shape.strokeBorder(accentColor.opacity(isDragging ? 0.85 : 0.35), lineWidth: 1.2)
                )
                .shadow(color: Color.black.opacity(0.38), radius: 4, x: 0, y: 2)
        }
    }

    private func updateValue(at x: CGFloat, totalWidth: CGFloat, thumbWidth: CGFloat) {
        let travelWidth = max(1, totalWidth - thumbWidth)
        let clampedX = min(max(x - thumbWidth * 0.5, 0), travelWidth)
        let ratio = Double(clampedX / travelWidth)
        var raw = range.lowerBound + ratio * (range.upperBound - range.lowerBound)
        if let step, step > 0 {
            raw = ((raw - range.lowerBound) / step).rounded() * step + range.lowerBound
        }
        let finalValue = min(max(raw, range.lowerBound), range.upperBound)
        if abs(finalValue - value) > 0.0001 {
            value = finalValue
        }
    }
}

// MARK: - Search & Text Input Fields with Crisp White Placeholders

/// Search input row with guaranteed high-contrast white placeholder text and clear button.
public struct NXLiquidGlassSearchField: View {
    public let placeholder: String
    @Binding public var text: String
    public var onClear: (() -> Void)?

    public init(_ placeholder: String, text: Binding<String>, onClear: (() -> Void)? = nil) {
        self.placeholder = placeholder
        self._text = text
        self.onClear = onClear
    }

    public var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 12, weight: .semibold))
                .foregroundColor(.white.opacity(0.92))

            ZStack(alignment: .leading) {
                if text.isEmpty {
                    Text(placeholder)
                        .font(.system(size: 12, weight: .medium, design: .rounded))
                        .foregroundColor(.white.opacity(0.86))
                        .lineLimit(1)
                        .allowsHitTesting(false)
                }

                TextField("", text: $text)
                    .textFieldStyle(.plain)
                    .font(.system(size: 12, weight: .medium, design: .rounded))
                    .foregroundColor(.white)
            }

            if !text.isEmpty {
                Button(action: {
                    text = ""
                    onClear?()
                }) {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 12))
                        .foregroundColor(.white.opacity(0.85))
                }
                .buttonStyle(NXPressableButtonStyle())
                .nxNoFocusRing()
            }
        }
    }
}

/// General single-line text field with crisp white placeholder text over glass platters.
public struct NXLiquidGlassTextField: View {
    public let placeholder: String
    @Binding public var text: String

    public init(_ placeholder: String, text: Binding<String>) {
        self.placeholder = placeholder
        self._text = text
    }

    public var body: some View {
        ZStack(alignment: .leading) {
            if text.isEmpty {
                Text(placeholder)
                    .font(.system(size: 12, weight: .medium, design: .rounded))
                    .foregroundColor(.white.opacity(0.78))
                    .lineLimit(1)
                    .allowsHitTesting(false)
            }

            TextField("", text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: 12, weight: .medium, design: .rounded))
                .foregroundColor(.white)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .nxGlassCard(cornerRadius: 9, isNested: true)
    }
}
