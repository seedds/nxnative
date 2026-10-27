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
    public static let textSecondary = Color(red: 0x9E / 255.0, green: 0x9E / 255.0, blue: 0xB2 / 255.0)
    public static let textMuted = Color(red: 0x65 / 255.0, green: 0x65 / 255.0, blue: 0x7A / 255.0)
    
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
public struct NXPrimaryButtonStyle: ButtonStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .bold, design: .rounded))
            .foregroundColor(.white)
            .padding(.horizontal, 16)
            .padding(.vertical, 9)
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(NXTheme.primaryGradient)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(Color.white.opacity(configuration.isPressed ? 0.4 : 0.2), lineWidth: 1)
            )
            .shadow(color: NXTheme.primary.opacity(configuration.isPressed ? 0.2 : 0.45), radius: 8, x: 0, y: 3)
            .scaleEffect(configuration.isPressed ? 0.98 : 1.0)
            .animation(.easeInOut(duration: 0.15), value: configuration.isPressed)
            .nxNoFocusRing()
    }
}

public struct NXSecondaryButtonStyle: ButtonStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 12, weight: .semibold, design: .rounded))
            .foregroundColor(NXTheme.textPrimary)
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(configuration.isPressed ? NXTheme.glassSurfaceActive : NXTheme.glassSurfaceElevated)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
            )
            .scaleEffect(configuration.isPressed ? 0.98 : 1.0)
            .animation(.easeInOut(duration: 0.15), value: configuration.isPressed)
            .nxNoFocusRing()
    }
}

public struct NXAccentButtonStyle: ButtonStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 12, weight: .bold, design: .rounded))
            .foregroundColor(.white)
            .padding(.horizontal, 14)
            .padding(.vertical, 7)
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(NXTheme.tertiary)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(Color.white.opacity(0.2), lineWidth: 0.8)
            )
            .shadow(color: NXTheme.tertiary.opacity(0.4), radius: 6, x: 0, y: 2)
            .scaleEffect(configuration.isPressed ? 0.98 : 1.0)
            .animation(.easeInOut(duration: 0.15), value: configuration.isPressed)
            .nxNoFocusRing()
    }
}
