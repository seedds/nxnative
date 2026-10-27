import SwiftUI
import AppKit

public struct CompatibilityView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var filteredGames: [CompatibleGameItem] {
        viewModel.currentFilteredCompatibleGames()
    }
    
    public var body: some View {
        VStack(spacing: 0) {
            // MARK: - Toolbar & Header
            CompatibilityToolbarView(viewModel: viewModel)
                .padding(.horizontal, 18)
                .padding(.vertical, 12)
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
            
            // MARK: - Filter Bar
            filterBar
            
            // MARK: - Main Games Scrollable View
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
                                        SoundEffectService.shared.play(.focus)
                                        viewModel.compatibilityFocusArea = .games
                                        viewModel.focusedCompatibilityIndex = index
                                    }
                                }
                            }
                            .padding(18)
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
                                        SoundEffectService.shared.play(.focus)
                                        viewModel.compatibilityFocusArea = .games
                                        viewModel.focusedCompatibilityIndex = index
                                    }
                                }
                            }
                            .padding(18)
                        }
                    }
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
    }
    
    private var filterBar: some View {
        HStack(spacing: 8) {
            filterPill("All", count: viewModel.compatibleGames.count)
            filterPill("Nintendo", count: viewModel.compatibleGames.filter { $0.engine.localizedCaseInsensitiveContains("nintendo") || $0.engine.localizedCaseInsensitiveContains("sead") }.count)
            filterPill("Unreal", count: viewModel.compatibleGames.filter { $0.engine.localizedCaseInsensitiveContains("unreal") }.count)
            filterPill("Unity", count: viewModel.compatibleGames.filter { $0.engine.localizedCaseInsensitiveContains("unity") }.count)
            filterPill("RE Engine", count: viewModel.compatibleGames.filter { $0.engine.localizedCaseInsensitiveContains("re engine") || $0.engine.localizedCaseInsensitiveContains("capcom") }.count)
            
            Spacer()
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 8)
        .background(NXTheme.glassSurface.opacity(0.35))
    }
    
    private func filterPill(_ name: String, count: Int) -> some View {
        let isSelected = viewModel.compatibilityFilter == name
        let isFocused = viewModel.compatibilityFocusArea == .filters && viewModel.compatibilityFilter == name
        return Button(action: {
            SoundEffectService.shared.play(.click)
            viewModel.compatibilityFocusArea = .filters
            viewModel.compatibilityFilter = name
        }) {
            HStack(spacing: 5) {
                Text(name)
                    .font(.system(size: 11, weight: isSelected ? .bold : .medium, design: .rounded))
                Text("(\(count))")
                    .font(.system(size: 10, design: .monospaced))
                    .opacity(0.8)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(
                ZStack {
                    if isSelected {
                        Capsule().fill(NXTheme.primaryGradient)
                    } else {
                        Capsule().fill(NXTheme.glassSurfaceElevated.opacity(0.6))
                    }
                }
            )
            .foregroundColor(.white)
            .clipShape(Capsule())
            .overlay(
                Capsule()
                    .strokeBorder(isSelected ? Color.white.opacity(0.4) : (isFocused ? NXTheme.secondary : NXTheme.borderSubtle), lineWidth: isFocused ? 1.5 : 0.8)
            )
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
    
    private var emptyFilterView: some View {
        VStack(spacing: 12) {
            Image(systemName: "tray")
                .font(.system(size: 38))
                .foregroundColor(NXTheme.secondary.opacity(0.4))
            Text("No compatible games match your filter.")
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(NXTheme.textSecondary)
        }
        .frame(maxWidth: .infinity, minHeight: 300)
        .padding(40)
    }
}
