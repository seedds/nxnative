import SwiftUI

// MARK: - Shared Liquid Glass Container

/// Wraps grouped glass controls in a shared `GlassEffectContainer` on macOS 26+
/// so sibling lenses blend, share backdrop sampling, and morph seamlessly.
public struct NXGlassContainer<Content: View>: View {
    private let spacing: CGFloat?
    private let content: Content

    public init(spacing: CGFloat? = 12, @ViewBuilder content: () -> Content) {
        self.spacing = spacing
        self.content = content()
    }

    public var body: some View {
        if #available(macOS 26.0, *) {
            GlassEffectContainer(spacing: spacing) {
                content
            }
        } else {
            content
        }
    }
}

// MARK: - Transition Kind

public enum NXGlassTransitionKind: Sendable {
    case matchedGeometry
    case materialize
    case identity
}

// MARK: - View Modifiers

private struct NXGlassBarModifier<S: InsettableShape>: ViewModifier {
    let shape: S
    let tint: Color?
    let fallbackFill: Color
    let fallbackStroke: Color

    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    func body(content: Content) -> some View {
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency
        if #available(macOS 26.0, *) {
            let resolvedTint = tint ?? NXTheme.glassSurface.opacity(glassEnv.mix(frosted: 0.56, liquid: 0.20))
            let activeGlass: Glass = useGlass ? Glass.clear.tint(resolvedTint) : .identity
            let frostOpacity = glassEnv.frostVeilFactor

            content
                .background(
                    Group {
                        if !useGlass {
                            shape.fill(fallbackFill)
                        } else if frostOpacity > 0.01 {
                            ZStack {
                                shape.fill(NXTheme.glassSurface.opacity(frostOpacity * 0.44))
                                shape.fill(.ultraThinMaterial).opacity(frostOpacity * 0.78)
                            }
                        }
                    }
                )
                .overlay(
                    shape.strokeBorder(
                        useGlass ? Color.white.opacity(glassEnv.mix(frosted: 0.09, liquid: 0.16)) : fallbackStroke,
                        lineWidth: useGlass ? 0.75 : 1.0
                    )
                )
                .glassEffect(activeGlass, in: shape)
                .shadow(
                    color: Color.black.opacity(useGlass ? glassEnv.mix(frosted: 0.34, liquid: 0.26) : 0.18),
                    radius: useGlass ? 14 : 8,
                    x: 0,
                    y: useGlass ? 5 : 2
                )
        } else {
            content
                .background(shape.fill(fallbackFill))
                .overlay(shape.strokeBorder(fallbackStroke, lineWidth: 1))
        }
    }
}

private struct NXGlassCardModifier: ViewModifier {
    let cornerRadius: CGFloat
    let tint: Color?
    let isNested: Bool
    let isInteractive: Bool

    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency

        if isNested {
            // Avoid glass-on-glass: nested platters inside a GlassCard use a lightweight concentric surface.
            content
                .background(
                    shape.fill(
                        useGlass
                            ? Color.white.opacity(glassEnv.mix(frosted: 0.075, liquid: 0.04))
                            : NXTheme.glassSurfaceElevated.opacity(0.65)
                    )
                )
                .overlay(
                    shape.strokeBorder(
                        useGlass ? Color.white.opacity(glassEnv.mix(frosted: 0.08, liquid: 0.12)) : NXTheme.borderSubtle.opacity(0.75),
                        lineWidth: 0.8
                    )
                )
        } else if #available(macOS 26.0, *) {
            let resolvedTint = tint ?? NXTheme.glassSurface.opacity(glassEnv.mix(frosted: 0.58, liquid: 0.22))
            let activeGlass: Glass = useGlass
                ? Glass.clear.tint(resolvedTint).interactive(isInteractive)
                : .identity
            let frostOpacity = glassEnv.frostVeilFactor

            content
                .background(
                    Group {
                        if !useGlass {
                            ZStack {
                                shape.fill(NXTheme.glassSurface.opacity(0.85))
                                shape.fill(.ultraThinMaterial)
                            }
                        } else if frostOpacity > 0.01 {
                            ZStack {
                                shape.fill(NXTheme.glassSurface.opacity(frostOpacity * 0.46))
                                shape.fill(.ultraThinMaterial).opacity(frostOpacity * 0.75)
                            }
                        }
                    }
                )
                .overlay(
                    shape.strokeBorder(
                        useGlass ? Color.white.opacity(glassEnv.mix(frosted: 0.09, liquid: 0.16)) : Color.clear,
                        lineWidth: 0.75
                    )
                )
                .overlay(
                    Group {
                        if !useGlass {
                            shape.strokeBorder(NXTheme.specularRim, lineWidth: 1)
                        }
                    }
                )
                .glassEffect(activeGlass, in: shape)
                .shadow(
                    color: Color.black.opacity(useGlass ? glassEnv.mix(frosted: 0.32, liquid: 0.24) : 0.35),
                    radius: useGlass ? 12 : 10,
                    x: 0,
                    y: 4
                )
        } else {
            content
                .background(
                    ZStack {
                        shape.fill(NXTheme.glassSurface.opacity(0.85))
                        shape.fill(.ultraThinMaterial)
                    }
                )
                .overlay(shape.strokeBorder(NXTheme.specularRim, lineWidth: 1))
                .shadow(color: Color.black.opacity(0.35), radius: 10, x: 0, y: 4)
        }
    }
}

private struct NXGlassInteractiveModifier<S: InsettableShape>: ViewModifier {
    let shape: S
    let tint: Color?
    let isSelected: Bool
    let isFocused: Bool
    let isHovered: Bool
    let isInteractive: Bool
    let fallbackFill: Color
    let fallbackSelectedFill: Color?

    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.isEnabled) private var isControlEnabled

    func body(content: Content) -> some View {
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency
        let interactiveActive = isInteractive && isControlEnabled

        if #available(macOS 26.0, *) {
            let resolvedTint: Color = {
                if let tint {
                    return tint
                }
                if isSelected {
                    return NXTheme.primary.opacity(glassEnv.mix(frosted: 0.78, liquid: 0.62))
                }
                if isFocused {
                    return NXTheme.primary.opacity(0.46)
                }
                if isHovered {
                    return Color.white.opacity(glassEnv.mix(frosted: 0.16, liquid: 0.12))
                }
                return NXTheme.glassSurface.opacity(glassEnv.mix(frosted: 0.44, liquid: 0.16))
            }()

            let activeGlass: Glass = useGlass
                ? Glass.clear.tint(resolvedTint).interactive(interactiveActive)
                : .identity

            let classicFill: Color = {
                if isSelected {
                    return fallbackSelectedFill ?? NXTheme.primary.opacity(0.22)
                }
                if isFocused || isHovered {
                    return NXTheme.glassSurfaceActive
                }
                return fallbackFill
            }()

            content
                .foregroundStyle(isSelected ? Color.white : Color.primary)
                .background(
                    Group {
                        if !useGlass {
                            shape.fill(classicFill)
                        } else if isSelected {
                            // Solid tinted base keeps selected content crisp white instead of washed-out
                            shape.fill(NXTheme.primary.opacity(glassEnv.mix(frosted: 0.72, liquid: 0.54)))
                        } else if glassEnv.frostVeilFactor > 0.01 {
                            shape.fill(NXTheme.glassSurfaceElevated.opacity(glassEnv.frostVeilFactor * 0.35))
                        }
                    }
                )
                .overlay(
                    Group {
                        if isFocused {
                            shape.strokeBorder(Color.white.opacity(0.95), lineWidth: 2.0)
                        } else if isSelected {
                            shape.strokeBorder(NXTheme.selectedBorder, lineWidth: useGlass ? 1.2 : 1.5)
                        } else if !useGlass {
                            shape.strokeBorder(
                                isHovered ? NXTheme.primary.opacity(0.45) : NXTheme.borderSubtle,
                                lineWidth: 1.0
                            )
                        } else if isHovered {
                            shape.strokeBorder(Color.white.opacity(0.24), lineWidth: 0.9)
                        }
                    }
                )
                .glassEffect(activeGlass, in: shape)
                .shadow(
                    color: isFocused
                        ? NXTheme.primary.opacity(0.55)
                        : (isSelected ? NXTheme.primary.opacity(useGlass ? 0.30 : 0.25) : Color.clear),
                    radius: isFocused ? 10 : 6,
                    x: 0,
                    y: 2
                )
        } else {
            let classicFill: Color = {
                if isSelected {
                    return fallbackSelectedFill ?? NXTheme.primary.opacity(0.22)
                }
                if isFocused || isHovered {
                    return NXTheme.glassSurfaceActive
                }
                return fallbackFill
            }()

            content
                .background(shape.fill(classicFill))
                .overlay(
                    Group {
                        if isFocused {
                            shape.strokeBorder(Color.white.opacity(0.95), lineWidth: 2.0)
                        } else if isSelected {
                            shape.strokeBorder(NXTheme.selectedBorder, lineWidth: 1.5)
                        } else {
                            shape.strokeBorder(
                                isHovered ? NXTheme.primary.opacity(0.45) : NXTheme.borderSubtle,
                                lineWidth: 1.0
                            )
                        }
                    }
                )
                .shadow(
                    color: isFocused ? NXTheme.primary.opacity(0.55) : Color.clear,
                    radius: 10,
                    x: 0,
                    y: 2
                )
        }
    }
}

private struct NXScrollEdgeEffectModifier: ViewModifier {
    let edges: Edge.Set
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            if glassEnv.isEnabled && !reduceTransparency {
                content.scrollEdgeEffectStyle(.soft, for: edges)
            } else {
                content.scrollEdgeEffectHidden(true, for: edges)
            }
        } else {
            content
        }
    }
}

private struct NXBackgroundExtensionModifier: ViewModifier {
    let isEnabled: Bool
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            content.backgroundExtensionEffect(isEnabled: isEnabled && glassEnv.isEnabled && !reduceTransparency)
        } else {
            content
        }
    }
}

private struct NXSheetSurfaceModifier: ViewModifier {
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    func body(content: Content) -> some View {
        let useGlass = glassEnv.isSupportedAndEnabled && !reduceTransparency
        content
            .background(
                ZStack {
                    if useGlass {
                        // Allow macOS 26's native Liquid Glass sheet backdrop to refract underneath,
                        // with an obsidian contrast veil calibrated continuously to Liquid vs Frosted intensity.
                        NXTheme.canvasObsidian.opacity(glassEnv.mix(frosted: 0.66, liquid: 0.32))
                        RadialGradient(
                            colors: [NXTheme.primary.opacity(glassEnv.mix(frosted: 0.08, liquid: 0.14)), Color.clear],
                            center: .topTrailing,
                            startRadius: 20,
                            endRadius: 360
                        )
                    } else {
                        NXTheme.canvasObsidian
                    }
                }
                .ignoresSafeArea()
            )
            .preferredColorScheme(.dark)
    }
}

private struct NXGlassTransitionModifier: ViewModifier {
    let kind: NXGlassTransitionKind
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            if reduceMotion {
                content.glassEffectTransition(.identity)
            } else {
                switch kind {
                case .matchedGeometry:
                    content.glassEffectTransition(.matchedGeometry)
                case .materialize:
                    content.glassEffectTransition(.materialize)
                case .identity:
                    content.glassEffectTransition(.identity)
                }
            }
        } else {
            content
        }
    }
}

// MARK: - Public View Extensions

public extension View {
    /// Styles a floating navigation bar, toolbar, or prompt dock with Liquid Glass.
    func nxGlassBar<S: InsettableShape>(
        in shape: S,
        tint: Color? = nil,
        fallbackFill: Color = NXTheme.glassSurface.opacity(0.92),
        fallbackStroke: Color = NXTheme.borderSubtle
    ) -> some View {
        modifier(
            NXGlassBarModifier(
                shape: shape,
                tint: tint,
                fallbackFill: fallbackFill,
                fallbackStroke: fallbackStroke
            )
        )
    }

    /// Styles a card container with Liquid Glass (or a concentric nested platter when `isNested` is true).
    func nxGlassCard(
        cornerRadius: CGFloat = 14,
        tint: Color? = nil,
        isNested: Bool = false,
        isInteractive: Bool = false
    ) -> some View {
        modifier(
            NXGlassCardModifier(
                cornerRadius: cornerRadius,
                tint: tint,
                isNested: isNested,
                isInteractive: isInteractive
            )
        )
    }

    /// Styles an interactive pill, tab, chip, swatch, or button with pointer/press Liquid Glass physics.
    /// Apply `.padding(...)` and `.frame(...)` BEFORE calling this modifier.
    func nxGlassInteractive<S: InsettableShape>(
        in shape: S,
        tint: Color? = nil,
        isSelected: Bool = false,
        isFocused: Bool = false,
        isHovered: Bool = false,
        isInteractive: Bool = true,
        fallbackFill: Color = NXTheme.glassSurfaceElevated,
        fallbackSelectedFill: Color? = nil
    ) -> some View {
        modifier(
            NXGlassInteractiveModifier(
                shape: shape,
                tint: tint,
                isSelected: isSelected,
                isFocused: isFocused,
                isHovered: isHovered,
                isInteractive: isInteractive,
                fallbackFill: fallbackFill,
                fallbackSelectedFill: fallbackSelectedFill
            )
        )
    }

    /// Associates a morphing identifier with a glass element inside an `NXGlassContainer`.
    @ViewBuilder
    func nxGlassID<ID: Hashable & Sendable>(_ id: ID?, in namespace: Namespace.ID) -> some View {
        if #available(macOS 26.0, *) {
            self.glassEffectID(id, in: namespace)
        } else {
            self
        }
    }

    /// Unions adjacent glass elements into a single continuous capsule inside an `NXGlassContainer`.
    @ViewBuilder
    func nxGlassUnion<ID: Hashable & Sendable>(id: ID?, namespace: Namespace.ID) -> some View {
        if #available(macOS 26.0, *) {
            self.glassEffectUnion(id: id, namespace: namespace)
        } else {
            self
        }
    }

    /// Configures insertion/removal morphing transitions for conditional glass elements.
    func nxGlassTransition(_ kind: NXGlassTransitionKind) -> some View {
        modifier(NXGlassTransitionModifier(kind: kind))
    }

    /// Applies soft scroll-edge optical refraction under floating Liquid Glass bars on macOS 26+.
    func nxScrollEdgeEffect(edges: Edge.Set = [.top, .bottom]) -> some View {
        modifier(NXScrollEdgeEffectModifier(edges: edges))
    }

    /// Extends hero/banner artwork optically beneath adjacent floating bars on macOS 26+.
    func nxBackgroundExtension(isEnabled: Bool = true) -> some View {
        modifier(NXBackgroundExtensionModifier(isEnabled: isEnabled))
    }

    /// Configures a modal `.sheet` root view to embrace macOS 26's native floating Liquid Glass sheet.
    func nxSheetSurface() -> some View {
        modifier(NXSheetSurfaceModifier())
    }
}
