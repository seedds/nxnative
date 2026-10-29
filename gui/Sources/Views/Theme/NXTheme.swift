import SwiftUI

public enum NXTheme {
    // MARK: - Dynamic Highlight Theme
    public static var activeHighlightTheme: LauncherHighlightTheme = {
        if let saved = UserDefaults.standard.string(forKey: "NXNative_SavedHighlightTheme"),
           let theme = LauncherHighlightTheme(rawValue: saved) {
            return theme
        }
        return .electricPurple
    }()

    // MARK: - Liquid Glass Global State
    public static var isLiquidGlassEnabled: Bool = {
        if let saved = UserDefaults.standard.object(forKey: "NXNative_SavedLiquidGlassEnabled") as? Bool {
            return saved
        }
        return true
    }()

    public static var liquidGlassAppearance: LiquidGlassAppearance = {
        if let saved = UserDefaults.standard.string(forKey: "NXNative_SavedLiquidGlassAppearance"),
           let mode = LiquidGlassAppearance(rawValue: saved) {
            return mode
        }
        return .liquid
    }()

    public static var liquidGlassIntensity: Double = {
        if let saved = UserDefaults.standard.object(forKey: "NXNative_SavedLiquidGlassIntensity") as? Double {
            return min(max(saved, 0.0), 1.0)
        }
        return liquidGlassAppearance == .liquid ? 1.0 : 0.0
    }()
    
    // MARK: - Core Palette
    public static var primary: Color { activeHighlightTheme.primaryColor }
    public static var secondary: Color { activeHighlightTheme.secondaryColor }
    public static let tertiary = Color(red: 0xAA / 255.0, green: 0x45 / 255.0, blue: 0x00 / 255.0)
    public static let canvasObsidian = Color(red: 0x14 / 255.0, green: 0x14 / 255.0, blue: 0x18 / 255.0)
    
    // MARK: - Liquid Glass Surfaces
    public static let glassSurface = Color(red: 0x1B / 255.0, green: 0x1B / 255.0, blue: 0x22 / 255.0)
    public static let glassSurfaceElevated = Color(red: 0x23 / 255.0, green: 0x23 / 255.0, blue: 0x2E / 255.0)
    public static let glassSurfaceActive = Color(red: 0x2C / 255.0, green: 0x28 / 255.0, blue: 0x3C / 255.0)
    public static let borderSubtle = Color(red: 0x2D / 255.0, green: 0x2D / 255.0, blue: 0x3C / 255.0)
    
    // MARK: - Typography Colors
    public static let textPrimary = Color.white
    public static let textSecondary = Color(red: 0xEE / 255.0, green: 0xF0 / 255.0, blue: 0xF8 / 255.0)
    public static let textMuted = Color(red: 0xCD / 255.0, green: 0xD1 / 255.0, blue: 0xE2 / 255.0)
    
    // MARK: - Gradients
    public static var primaryGradient: LinearGradient {
        LinearGradient(
            colors: [primary, secondary],
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
    }
    
    public static var logoGradient: LinearGradient {
        LinearGradient(
            colors: [primary, Color(red: 0xF0 / 255.0, green: 0x3B / 255.0, blue: 0x3B / 255.0)],
            startPoint: .leading,
            endPoint: .trailing
        )
    }
    
    public static let specularRim = LinearGradient(
        colors: [
            Color.white.opacity(0.18),
            Color.white.opacity(0.06),
            Color.white.opacity(0.02)
        ],
        startPoint: .topLeading,
        endPoint: .bottomTrailing
    )
    
    public static var selectedBorder: LinearGradient {
        LinearGradient(
            colors: [
                secondary,
                primary
            ],
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
    }
    
    public static var specularRimHover: LinearGradient {
        LinearGradient(
            colors: [
                secondary.opacity(0.6),
                primary.opacity(0.3),
                Color.white.opacity(0.05)
            ],
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
    }
}

public extension Color {
    static var nxPrimary: Color { NXTheme.primary }
    static var nxSecondary: Color { NXTheme.secondary }
    static let nxTertiary = NXTheme.tertiary
    static let nxCanvas = NXTheme.canvasObsidian
    static let nxSurface = NXTheme.glassSurface
    static let nxSurfaceElevated = NXTheme.glassSurfaceElevated
    static let nxBorder = NXTheme.borderSubtle
    static let nxTextPrimary = NXTheme.textPrimary
    static let nxTextSecondary = NXTheme.textSecondary
    static let nxTextMuted = NXTheme.textMuted
}

public extension View {
    /// Removes macOS system blue selection / focus ring around buttons and controls
    @ViewBuilder
    func nxNoFocusRing() -> some View {
        if #available(macOS 14.0, *) {
            self.focusEffectDisabled()
                .focusable(false)
        } else {
            self.focusable(false)
        }
    }
}

// MARK: - Button Styles
public struct NXPressableButtonStyle: ButtonStyle {
    public init() {}

    public func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.95 : 1.0)
            .opacity(configuration.isPressed ? 0.82 : 1.0)
            .brightness(configuration.isPressed ? 0.05 : 0.0)
            .animation(.spring(response: 0.20, dampingFraction: 0.70), value: configuration.isPressed)
    }
}

public struct NXPrimaryButtonStyle: ButtonStyle {
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.isEnabled) private var isControlEnabled

    public init() {}

    public func makeBody(configuration: Configuration) -> some View {
        let shape = RoundedRectangle(cornerRadius: 10, style: .continuous)
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency

        if #available(macOS 26.0, *) {
            let baseGlass: Glass = (glassEnv.intensity >= 0.5) ? .clear : .regular
            let activeGlass: Glass = useGlass
                ? baseGlass.tint(NXTheme.primary.opacity(glassEnv.mix(frosted: 0.90, liquid: 0.76))).interactive(isControlEnabled)
                : .identity

            configuration.label
                .font(.system(size: 13, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 16)
                .padding(.vertical, 9)
                .background(
                    Group {
                        if !useGlass {
                            shape.fill(NXTheme.primaryGradient)
                        } else {
                            shape.fill(
                                LinearGradient(
                                    colors: [
                                        NXTheme.primary.opacity(glassEnv.mix(frosted: 0.86, liquid: 0.68)),
                                        NXTheme.secondary.opacity(glassEnv.mix(frosted: 0.74, liquid: 0.52))
                                    ],
                                    startPoint: .topLeading,
                                    endPoint: .bottomTrailing
                                )
                            )
                        }
                    }
                )
                .overlay(
                    shape.strokeBorder(
                        Color.white.opacity(configuration.isPressed ? 0.60 : (useGlass ? 0.32 : 0.24)),
                        lineWidth: configuration.isPressed ? 1.4 : 1.0
                    )
                )
                .overlay(
                    shape.fill(Color.white.opacity(configuration.isPressed ? 0.12 : 0.0))
                )
                .glassEffect(activeGlass, in: shape)
                .shadow(
                    color: NXTheme.primary.opacity(useGlass ? 0.34 : (configuration.isPressed ? 0.2 : 0.45)),
                    radius: useGlass ? 10 : 8,
                    x: 0,
                    y: 3
                )
                .scaleEffect(configuration.isPressed ? 0.96 : 1.0)
                .brightness(configuration.isPressed ? -0.04 : 0.0)
                .opacity(isControlEnabled ? 1.0 : 0.48)
                .animation(.spring(response: 0.20, dampingFraction: 0.70), value: configuration.isPressed)
                .nxNoFocusRing()
        } else {
            configuration.label
                .font(.system(size: 13, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 16)
                .padding(.vertical, 9)
                .background(shape.fill(NXTheme.primaryGradient))
                .overlay(shape.strokeBorder(Color.white.opacity(configuration.isPressed ? 0.5 : 0.2), lineWidth: 1))
                .shadow(color: NXTheme.primary.opacity(configuration.isPressed ? 0.2 : 0.45), radius: 8, x: 0, y: 3)
                .scaleEffect(configuration.isPressed ? 0.96 : 1.0)
                .opacity(isControlEnabled ? 1.0 : 0.48)
                .animation(.spring(response: 0.20, dampingFraction: 0.70), value: configuration.isPressed)
                .nxNoFocusRing()
        }
    }
}

public struct NXSecondaryButtonStyle: ButtonStyle {
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.isEnabled) private var isControlEnabled

    public init() {}

    public func makeBody(configuration: Configuration) -> some View {
        let shape = RoundedRectangle(cornerRadius: 9, style: .continuous)
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency

        if #available(macOS 26.0, *) {
            let baseGlass: Glass = (glassEnv.intensity >= 0.5) ? .clear : .regular
            let tintColor: Color = NXTheme.glassSurfaceElevated.opacity(glassEnv.mix(frosted: 0.48, liquid: 0.18))
            let activeGlass: Glass = useGlass
                ? baseGlass.tint(tintColor).interactive(isControlEnabled)
                : .identity

            configuration.label
                .font(.system(size: 12, weight: .semibold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .background(
                    Group {
                        if !useGlass {
                            shape.fill(configuration.isPressed ? NXTheme.glassSurfaceActive : NXTheme.glassSurfaceElevated)
                        } else {
                            shape.fill(
                                Color.white.opacity(configuration.isPressed ? 0.12 : glassEnv.mix(frosted: 0.10, liquid: 0.035))
                            )
                        }
                    }
                )
                .overlay(
                    shape.strokeBorder(
                        Color.white.opacity(configuration.isPressed ? 0.42 : (useGlass ? 0.20 : 0.12)),
                        lineWidth: configuration.isPressed ? 1.2 : 0.8
                    )
                )
                .glassEffect(activeGlass, in: shape)
                .scaleEffect(configuration.isPressed ? 0.96 : 1.0)
                .opacity(isControlEnabled ? 1.0 : 0.48)
                .animation(.spring(response: 0.20, dampingFraction: 0.70), value: configuration.isPressed)
                .nxNoFocusRing()
        } else {
            configuration.label
                .font(.system(size: 12, weight: .semibold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .background(shape.fill(configuration.isPressed ? NXTheme.glassSurfaceActive : NXTheme.glassSurfaceElevated))
                .overlay(shape.strokeBorder(Color.white.opacity(configuration.isPressed ? 0.35 : 0.15), lineWidth: 0.8))
                .scaleEffect(configuration.isPressed ? 0.96 : 1.0)
                .opacity(isControlEnabled ? 1.0 : 0.48)
                .animation(.spring(response: 0.20, dampingFraction: 0.70), value: configuration.isPressed)
                .nxNoFocusRing()
        }
    }
}

public struct NXAccentButtonStyle: ButtonStyle {
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.isEnabled) private var isControlEnabled

    public init() {}

    public func makeBody(configuration: Configuration) -> some View {
        let shape = RoundedRectangle(cornerRadius: 9, style: .continuous)
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency

        if #available(macOS 26.0, *) {
            let baseGlass: Glass = (glassEnv.intensity >= 0.5) ? .clear : .regular
            let activeGlass: Glass = useGlass
                ? baseGlass.tint(NXTheme.tertiary.opacity(glassEnv.mix(frosted: 0.90, liquid: 0.76))).interactive(isControlEnabled)
                : .identity

            configuration.label
                .font(.system(size: 12, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 14)
                .padding(.vertical, 7)
                .background(
                    Group {
                        if !useGlass {
                            shape.fill(NXTheme.tertiary)
                        } else {
                            shape.fill(
                                LinearGradient(
                                    colors: [
                                        NXTheme.tertiary.opacity(glassEnv.mix(frosted: 0.88, liquid: 0.74)),
                                        NXTheme.tertiary.opacity(glassEnv.mix(frosted: 0.72, liquid: 0.54))
                                    ],
                                    startPoint: .topLeading,
                                    endPoint: .bottomTrailing
                                )
                            )
                        }
                    }
                )
                .overlay(
                    shape.strokeBorder(
                        Color.white.opacity(configuration.isPressed ? 0.50 : 0.24),
                        lineWidth: configuration.isPressed ? 1.2 : 0.8
                    )
                )
                .glassEffect(activeGlass, in: shape)
                .shadow(color: NXTheme.tertiary.opacity(useGlass ? 0.32 : 0.4), radius: 6, x: 0, y: 2)
                .scaleEffect(configuration.isPressed ? 0.96 : 1.0)
                .opacity(isControlEnabled ? 1.0 : 0.48)
                .animation(.spring(response: 0.20, dampingFraction: 0.70), value: configuration.isPressed)
                .nxNoFocusRing()
        } else {
            configuration.label
                .font(.system(size: 12, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 14)
                .padding(.vertical, 7)
                .background(shape.fill(NXTheme.tertiary))
                .overlay(shape.strokeBorder(Color.white.opacity(configuration.isPressed ? 0.4 : 0.2), lineWidth: 0.8))
                .shadow(color: NXTheme.tertiary.opacity(0.4), radius: 6, x: 0, y: 2)
                .scaleEffect(configuration.isPressed ? 0.96 : 1.0)
                .opacity(isControlEnabled ? 1.0 : 0.48)
                .animation(.spring(response: 0.20, dampingFraction: 0.70), value: configuration.isPressed)
                .nxNoFocusRing()
        }
    }
}
