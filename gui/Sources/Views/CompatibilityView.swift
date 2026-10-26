import SwiftUI
import AppKit

public struct CompatibilityView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var filteredGames: [CompatibleGameItem] {
        var list = viewModel.compatibleGames
        
        let query = viewModel.compatibilitySearchText.trimmingCharacters(in: .whitespaces).lowercased()
        if !query.isEmpty {
            list = list.filter {
                $0.title.lowercased().contains(query) ||
                $0.titleId.lowercased().contains(query) ||
                $0.engine.lowercased().contains(query) ||
                $0.graphicsApi.lowercased().contains(query)
            }
        }
        
        switch viewModel.compatibilityFilter {
        case "Unreal":
            list = list.filter { $0.engine.localizedCaseInsensitiveContains("unreal") }
        case "Unity":
            list = list.filter { $0.engine.localizedCaseInsensitiveContains("unity") }
        case "Nintendo":
            list = list.filter { $0.engine.localizedCaseInsensitiveContains("nintendo") || $0.engine.localizedCaseInsensitiveContains("sead") }
        case "RE Engine":
            list = list.filter { $0.engine.localizedCaseInsensitiveContains("re engine") || $0.engine.localizedCaseInsensitiveContains("capcom") }
        default:
            break
        }
        
        return list
    }
    
    public var body: some View {
        VStack(spacing: 0) {
            // MARK: - Toolbar & Header
            HStack(spacing: 14) {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 8) {
                        Text("Compatible Games")
                            .font(.system(size: 16, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        
                        Text("\(viewModel.compatibleGames.count) verified")
                            .font(.system(size: 10, weight: .bold, design: .monospaced))
                            .padding(.horizontal, 7)
                            .padding(.vertical, 2)
                            .background(NXTheme.primary.opacity(0.18))
                            .foregroundColor(NXTheme.secondary)
                            .clipShape(Capsule())
                    }
                    
                    HStack(spacing: 6) {
                        Circle()
                            .fill(viewModel.isCompatibilityFromOnline ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.tertiary)
                            .frame(width: 6, height: 6)
                        Text(viewModel.isCompatibilityFromOnline ? "Live GitHub Sync" : "Local Repository Cache")
                            .font(.system(size: 10, weight: .medium))
                            .foregroundColor(NXTheme.textSecondary)
                        
                        if let date = viewModel.compatibilityLastUpdated {
                            Text("• Updated \(formattedTime(date))")
                                .font(.system(size: 10))
                                .foregroundColor(NXTheme.textMuted)
                        }
                    }
                }
                
                Spacer()
                
                // Search Field
                HStack(spacing: 6) {
                    Image(systemName: "magnifyingglass")
                        .foregroundColor(NXTheme.textSecondary)
                    TextField("Search title, ID, engine...", text: $viewModel.compatibilitySearchText)
                        .textFieldStyle(.plain)
                        .foregroundColor(NXTheme.textPrimary)
                    if !viewModel.compatibilitySearchText.isEmpty {
                        Button(action: { viewModel.compatibilitySearchText = "" }) {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundColor(NXTheme.textMuted)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(
                    ZStack {
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .fill(NXTheme.glassSurfaceElevated.opacity(0.8))
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .fill(.ultraThinMaterial)
                    }
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
                )
                .frame(maxWidth: 240)
                
                // Refresh Button
                Button(action: {
                    viewModel.loadCompatibleGames(forceRefresh: true)
                }) {
                    HStack(spacing: 4) {
                        if viewModel.isLoadingCompatibility {
                            ProgressView()
                                .controlSize(.small)
                        } else {
                            Image(systemName: "arrow.clockwise")
                        }
                        Text("Sync")
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
                .disabled(viewModel.isLoadingCompatibility)
                
                // External GitHub Link
                Button(action: {
                    if let url = URL(string: "https://github.com/seedds/nxnative/blob/main/dm/games/README.md#games") {
                        NSWorkspace.shared.open(url)
                    }
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: "arrow.up.right.square")
                        Text("GitHub")
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
            }
            .padding(.horizontal, 18)
            .padding(.vertical, 12)
            .background(
                ZStack {
                    NXTheme.canvasObsidian.opacity(0.6)
                    Rectangle().fill(.ultraThinMaterial)
                }
            )
            .overlay(
                Rectangle()
                    .frame(height: 1)
                    .foregroundColor(NXTheme.borderSubtle),
                alignment: .bottom
            )
            
            // Filter Pills Row
            HStack(spacing: 8) {
                filterPill("All", count: viewModel.compatibleGames.count)
                filterPill("Unreal", count: viewModel.compatibleGames.filter { $0.engine.localizedCaseInsensitiveContains("unreal") }.count)
                filterPill("Unity", count: viewModel.compatibleGames.filter { $0.engine.localizedCaseInsensitiveContains("unity") }.count)
                filterPill("Nintendo", count: viewModel.compatibleGames.filter { $0.engine.localizedCaseInsensitiveContains("nintendo") || $0.engine.localizedCaseInsensitiveContains("sead") }.count)
                filterPill("RE Engine", count: viewModel.compatibleGames.filter { $0.engine.localizedCaseInsensitiveContains("re engine") || $0.engine.localizedCaseInsensitiveContains("capcom") }.count)
                Spacer()
            }
            .padding(.horizontal, 18)
            .padding(.vertical, 8)
            .background(NXTheme.canvasObsidian.opacity(0.4))
            
            Divider().opacity(0.2)
            
            // MARK: - Games List
            ScrollView {
                if filteredGames.isEmpty {
                    VStack(spacing: 12) {
                        Image(systemName: "magnifyingglass")
                            .font(.system(size: 38))
                            .foregroundColor(NXTheme.secondary.opacity(0.4))
                        Text("No compatible games match your filter.")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    .frame(maxWidth: .infinity, minHeight: 300)
                    .padding(40)
                } else {
                    LazyVStack(spacing: 8) {
                        ForEach(filteredGames, id: \.id) { game in
                            CompatibleGameRow(game: game) {
                                viewModel.selectedTab = 0
                            }
                            .id(game.id)
                        }
                    }
                    .padding(18)
                }
            }
        }
    }
    
    private func filterPill(_ name: String, count: Int) -> some View {
        let isSelected = viewModel.compatibilityFilter == name
        return Button(action: {
            viewModel.compatibilityFilter = name
        }) {
            HStack(spacing: 5) {
                Text(name)
                    .font(.system(size: 11, weight: isSelected ? .bold : .medium, design: .rounded))
                Text("(\(count))")
                    .font(.system(size: 10, design: .monospaced))
                    .opacity(0.7)
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
            .foregroundColor(isSelected ? .white : NXTheme.textSecondary)
            .clipShape(Capsule())
            .overlay(
                Capsule()
                    .strokeBorder(isSelected ? Color.white.opacity(0.25) : NXTheme.borderSubtle, lineWidth: 0.8)
            )
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
    
    private func formattedTime(_ date: Date) -> String {
        let f = DateFormatter()
        f.timeStyle = .short
        return f.string(from: date)
    }
}
