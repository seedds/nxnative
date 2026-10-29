import SwiftUI
import AppKit

public struct CompatibilityView: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var filteredGames: [CompatibleGameItem] {
        viewModel.currentFilteredCompatibleGames()
    }
    
    private var useFloatingGlass: Bool {
        glassEnv.isSupportedAndEnabled && !reduceTransparency
    }
    
    public var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                if filteredGames.isEmpty {
                    emptyFilterView
                } else {
                    if viewModel.compatibilityDisplayMode == .list {
                        LazyVStack(spacing: 8) {
                            ForEach(Array(filteredGames.enumerated()), id: \.element.id) { index, game in
                                let isFocused = viewModel.compatibilityFocusArea == .games && viewModel.focusedCompatibilityIndex == index
                                CompatibleGameRow(
                                    game: game,
                                    isRowFocused: isFocused,
                                    focusedButton: isFocused ? viewModel.focusedCompatibilityButtonIndex : nil,
                                    onNotes: {
                                        if let url = game.githubUrl { NSWorkspace.shared.open(url) }
                                    }
                                )
                                .id(game.id)
                                .onTapGesture {
                                    guard viewModel.focusedCompatibilityIndex != index || viewModel.compatibilityFocusArea != .games else { return }
                                    SoundEffectService.shared.play(.focus)
                                    viewModel.compatibilityFocusArea = .games
                                    viewModel.focusedCompatibilityIndex = index
                                }
                            }
                        }
                        .padding(.horizontal, 18)
                        .padding(.top, 14)
                        .padding(.bottom, 32)
                    } else {
                        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 12, alignment: .top), count: 6), spacing: 14) {
                            ForEach(Array(filteredGames.enumerated()), id: \.element.id) { index, game in
                                let isFocused = viewModel.compatibilityFocusArea == .games && viewModel.focusedCompatibilityIndex == index
                                CompatibleGameGridCard(
                                    item: game,
                                    isCardFocused: isFocused,
                                    focusedButton: isFocused ? viewModel.focusedCompatibilityButtonIndex : nil,
                                    onNotes: {
                                        if let url = game.githubUrl { NSWorkspace.shared.open(url) }
                                    }
                                )
                                .id(game.id)
                                .onTapGesture {
                                    guard viewModel.focusedCompatibilityIndex != index || viewModel.compatibilityFocusArea != .games else { return }
                                    SoundEffectService.shared.play(.focus)
                                    viewModel.compatibilityFocusArea = .games
                                    viewModel.focusedCompatibilityIndex = index
                                }
                            }
                        }
                        .padding(.horizontal, 16)
                        .padding(.top, 14)
                        .padding(.bottom, 32)
                    }
                }
            }
            .nxScrollEdgeEffect(edges: [.top, .bottom])
            .safeAreaInset(edge: .top, spacing: 0) {
                floatingHeaderStack
            }
            .onChange(of: viewModel.focusedCompatibilityIndex) { newIndex in
                if viewModel.compatibilityFocusArea == .games, newIndex >= 0, newIndex < filteredGames.count {
                    withAnimation(.easeInOut(duration: 0.2)) {
                        proxy.scrollTo(filteredGames[newIndex].id, anchor: .center)
                    }
                }
            }
        }
    }
    
    private var floatingHeaderStack: some View {
        VStack(spacing: useFloatingGlass ? 8 : 0) {
            CompatibilityToolbarView(viewModel: viewModel)
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .modifier(CompatibilityFloatingPanelModifier(viewModel: viewModel, useFloatingGlass: useFloatingGlass))
            
            CompatibilityFilterBarView(viewModel: viewModel)
                .modifier(CompatibilityFloatingFilterModifier(viewModel: viewModel, useFloatingGlass: useFloatingGlass))
        }
        .padding(.horizontal, useFloatingGlass ? 16 : 0)
        .padding(.top, useFloatingGlass ? 4 : 0)
        .padding(.bottom, useFloatingGlass ? 6 : 0)
    }
    
    private var emptyFilterView: some View {
        VStack(spacing: 12) {
            Image(systemName: "tray")
                .font(.system(size: 38))
                .foregroundColor(NXTheme.secondary.opacity(0.4))
            Text(viewModel.tr("compatibility.no_match"))
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(NXTheme.textSecondary)
        }
        .frame(maxWidth: .infinity, minHeight: 300)
        .padding(40)
    }
}

private struct CompatibilityFloatingPanelModifier: ViewModifier {
    @ObservedObject var viewModel: AppViewModel
    let useFloatingGlass: Bool
    
    func body(content: Content) -> some View {
        let intensity = viewModel.liquidGlassIntensity
        let bgOpacity = 0.40 * (1.0 - intensity * 0.70)
        let matOpacity = 0.85 * (1.0 - intensity * 0.60)
        let strokeOpacity = 0.09 + 0.12 * intensity
        
        content
            .background(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .fill(NXTheme.canvasObsidian.opacity(bgOpacity))
            )
            .background(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .fill(.ultraThinMaterial)
                    .opacity(matOpacity)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .strokeBorder(
                        LinearGradient(
                            colors: [
                                Color.white.opacity(strokeOpacity),
                                Color.white.opacity(strokeOpacity * 0.3),
                                Color.white.opacity(strokeOpacity * 0.7)
                            ],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        ),
                        lineWidth: 0.75
                    )
            )
            .shadow(
                color: Color.black.opacity(0.15 + 0.15 * (1.0 - intensity)),
                radius: 12,
                x: 0,
                y: 5
            )
    }
}

private struct CompatibilityFloatingFilterModifier: ViewModifier {
    @ObservedObject var viewModel: AppViewModel
    let useFloatingGlass: Bool
    
    func body(content: Content) -> some View {
        let intensity = viewModel.liquidGlassIntensity
        let bgOpacity = 0.32 * (1.0 - intensity * 0.80)
        let matOpacity = 0.80 * (1.0 - intensity * 0.65)
        let strokeOpacity = 0.08 + 0.12 * intensity
        
        content
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(NXTheme.glassSurface.opacity(bgOpacity))
            )
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(.ultraThinMaterial)
                    .opacity(matOpacity)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(
                        LinearGradient(
                            colors: [
                                Color.white.opacity(strokeOpacity),
                                Color.white.opacity(strokeOpacity * 0.3),
                                Color.white.opacity(strokeOpacity * 0.7)
                            ],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        ),
                        lineWidth: 0.75
                    )
            )
            .shadow(
                color: Color.black.opacity(0.12 + 0.12 * (1.0 - intensity)),
                radius: 10,
                x: 0,
                y: 4
            )
    }
}

