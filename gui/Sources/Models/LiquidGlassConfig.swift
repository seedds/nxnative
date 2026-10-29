import SwiftUI

/// Visual style variants for Liquid Glass across the launcher interface.
public enum LiquidGlassAppearance: String, CaseIterable, Identifiable, Sendable {
    /// High-translucency optical lens using `Glass.clear` with vivid refraction and specular rims.
    case liquid = "liquid"
    /// High-contrast satin diffused glass using `Glass.regular` with deeper surface attenuation.
    case frosted = "frosted"

    public var id: String { rawValue }

    public var iconName: String {
        switch self {
        case .liquid:
            return "drop.circle.fill"
        case .frosted:
            return "aqi.medium"
        }
    }

    public var titleLocalizationKey: String {
        switch self {
        case .liquid:
            return "personalization.glass_liquid"
        case .frosted:
            return "personalization.glass_frosted"
        }
    }

    public var descriptionLocalizationKey: String {
        switch self {
        case .liquid:
            return "personalization.glass_liquid_desc"
        case .frosted:
            return "personalization.glass_frosted_desc"
        }
    }
}

/// Reactive environment state propagating Liquid Glass configuration to all views and styles.
public struct NXLiquidGlassEnvironment: Equatable, Sendable {
    public var isEnabled: Bool
    public var appearance: LiquidGlassAppearance
    /// Continuous liquidity degree from `0.0` (100% Frosted / satin) to `1.0` (100% Liquid / clear refraction).
    public var intensity: Double
    public var highlightThemeRawValue: String

    public init(
        isEnabled: Bool = true,
        appearance: LiquidGlassAppearance = .liquid,
        intensity: Double? = nil,
        highlightTheme: LauncherHighlightTheme = NXTheme.activeHighlightTheme
    ) {
        self.isEnabled = isEnabled
        self.appearance = appearance
        let fallbackIntensity = (appearance == .liquid) ? 1.0 : 0.0
        self.intensity = min(max(intensity ?? fallbackIntensity, 0.0), 1.0)
        self.highlightThemeRawValue = highlightTheme.rawValue
    }

    /// Linearly interpolates a parameter between its Frosted (`0.0`) and Liquid (`1.0`) values.
    public func mix(frosted: Double, liquid: Double) -> Double {
        let t = min(max(intensity, 0.0), 1.0)
        return frosted + (liquid - frosted) * t
    }

    /// Frosted diffusion veil opacity (`1.0` at `intensity = 0.0`, `0.0` at `intensity = 1.0`).
    public var frostVeilFactor: Double {
        1.0 - min(max(intensity, 0.0), 1.0)
    }

    public var isSupportedAndEnabled: Bool {
        if #available(macOS 26.0, *) {
            return isEnabled
        }
        return false
    }
}

private struct NXLiquidGlassEnvironmentKey: EnvironmentKey {
    static var defaultValue: NXLiquidGlassEnvironment {
        NXLiquidGlassEnvironment(
            isEnabled: NXTheme.isLiquidGlassEnabled,
            appearance: NXTheme.liquidGlassAppearance,
            intensity: NXTheme.liquidGlassIntensity,
            highlightTheme: NXTheme.activeHighlightTheme
        )
    }
}

public extension EnvironmentValues {
    var nxLiquidGlass: NXLiquidGlassEnvironment {
        get { self[NXLiquidGlassEnvironmentKey.self] }
        set { self[NXLiquidGlassEnvironmentKey.self] = newValue }
    }
}
