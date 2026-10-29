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
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 180, maximum: 220), spacing: 18, alignment: .top)], spacing: 18) {
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
                        .padding(.horizontal, 18)
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
                .modifier(CompatibilityFloatingPanelModifier(useFloatingGlass: useFloatingGlass))
            
            CompatibilityFilterBarView(viewModel: viewModel)
                .modifier(CompatibilityFloatingFilterModifier(useFloatingGlass: useFloatingGlass))
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
    let useFloatingGlass: Bool
    
    func body(content: Content) -> some View {
        if useFloatingGlass {
            content.nxGlassBar(in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        } else {
            content
                .background(
                    ZStack {
                        NXTheme.canvasObsidian.opacity(0.8)
                        Rectangle().fill(.ultraThinMaterial)
                    }
                )
                .overlay(
                    Rectangle()
                        .frame(height: 1)
                        .foregroundColor(NXTheme.borderSubtle),
                    alignment: .bottom
                )
        }
    }
}

private struct CompatibilityFloatingFilterModifier: ViewModifier {
    let useFloatingGlass: Bool
    
    func body(content: Content) -> some View {
        if useFloatingGlass {
            content.nxGlassBar(in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        } else {
            content
                .background(NXTheme.glassSurface.opacity(0.35))
                .overlay(
                    Rectangle()
                        .frame(height: 1)
                        .foregroundColor(NXTheme.borderSubtle.opacity(0.6)),
                    alignment: .bottom
                )
        }
    }
}
