import SwiftUI
import AppKit

public struct CompatibilityToolbarView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
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
                        .foregroundColor(.white)
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
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        viewModel.compatibilitySearchText = ""
                    }) {
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
            
            displayModeToggle
            
            Button(action: {
                SoundEffectService.shared.play(.click)
                viewModel.loadCompatibleGames(forceRefresh: true)
            }) {
                HStack(spacing: 5) {
                    if viewModel.isLoadingCompatibility {
                        ProgressView().controlSize(.small)
                    } else {
                        Image(systemName: "arrow.clockwise")
                    }
                    Text("Refresh")
                }
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .controlSize(.small)
        }
    }
    
    private var displayModeToggle: some View {
        HStack(spacing: 2) {
            Button(action: {
                SoundEffectService.shared.play(.click)
                viewModel.compatibilityDisplayMode = .list
            }) {
                Image(systemName: "list.bullet")
                    .font(.system(size: 11, weight: viewModel.compatibilityDisplayMode == .list ? .bold : .semibold))
                    .foregroundColor(.white)
                    .padding(.horizontal, 7)
                    .padding(.vertical, 5)
                    .background(viewModel.compatibilityDisplayMode == .list ? NXTheme.primary.opacity(0.5) : Color.clear)
                    .cornerRadius(6)
            }
            .buttonStyle(.plain)
            .help("List View")
            
            Button(action: {
                SoundEffectService.shared.play(.click)
                viewModel.compatibilityDisplayMode = .grid
            }) {
                Image(systemName: "square.grid.2x2")
                    .font(.system(size: 11, weight: viewModel.compatibilityDisplayMode == .grid ? .bold : .semibold))
                    .foregroundColor(.white)
                    .padding(.horizontal, 7)
                    .padding(.vertical, 5)
                    .background(viewModel.compatibilityDisplayMode == .grid ? NXTheme.primary.opacity(0.5) : Color.clear)
                    .cornerRadius(6)
            }
            .buttonStyle(.plain)
            .help("Grid View")
        }
        .padding(2)
        .background(NXTheme.glassSurfaceElevated.opacity(0.6))
        .cornerRadius(8)
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }
    
    private func formattedTime(_ date: Date) -> String {
        let f = DateFormatter()
        f.timeStyle = .short
        return f.string(from: date)
    }
}
