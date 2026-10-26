import SwiftUI

public struct BuildGameSection: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label("1. Game Files (NSP / NSZ / XCI / ZIP)", systemImage: "gamecontroller.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 14, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    Spacer()
                }
                
                // Drag & Drop Area
                DropZoneView(viewModel: viewModel)
                
                if viewModel.selectedGameFolder != nil {
                    if viewModel.detectedFiles.isEmpty {
                        noFilesWarning
                    } else {
                        detectedGamesList
                    }
                }
                
                // Storage Space Status Alert
                storageStatusView
                
                // Continue to Step 2 Action
                if viewModel.selectedGame != nil {
                    HStack {
                        Spacer()
                        Button(action: {
                            withAnimation(.easeInOut(duration: 0.25)) {
                                viewModel.goToWizardStep(2)
                            }
                        }) {
                            HStack(spacing: 8) {
                                Text("Continue to Configuration")
                                Image(systemName: "arrow.right.circle.fill")
                            }
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .padding(.horizontal, 18)
                            .padding(.vertical, 8)
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    }
                    .padding(.top, 4)
                }
            }
        }
    }
    
    private var noFilesWarning: some View {
        HStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(NXTheme.tertiary)
            Text("No .nsp, .nsz, or .xci files found in this folder.")
                .font(.system(size: 12))
                .foregroundColor(NXTheme.textSecondary)
        }
        .padding(8)
        .background(NXTheme.tertiary.opacity(0.12))
        .cornerRadius(8)
    }
    
    private var detectedGamesList: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Select Game to Port (\(viewModel.detectedGames.count) detected):")
                    .font(.system(size: 12, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Spacer()
                if let sel = viewModel.selectedGame {
                    Text("Selected: \(sel.formattedSize)")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(NXTheme.secondary)
                }
            }
            
            VStack(spacing: 6) {
                ForEach(viewModel.detectedGames) { game in
                    SelectableGameCard(
                        game: game,
                        isSelected: viewModel.selectedGame?.id == game.id,
                        onSelect: {
                            viewModel.selectedGame = game
                        }
                    )
                }
            }
        }
        .padding(.top, 4)
    }
    
    @ViewBuilder
    private var storageStatusView: some View {
        if let diskWarning = viewModel.diskSpaceWarningMessage {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "externaldrive.badge.xmark")
                    .font(.system(size: 20))
                    .foregroundColor(NXTheme.tertiary)
                
                VStack(alignment: .leading, spacing: 2) {
                    Text("Low Disk Space Alert")
                        .font(.system(size: 12, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.tertiary)
                    Text(diskWarning)
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.textSecondary)
                }
            }
            .padding(10)
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(NXTheme.tertiary.opacity(0.12))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(NXTheme.tertiary.opacity(0.35), lineWidth: 1)
            )
        } else if let avail = viewModel.availableDiskBytes, let sel = viewModel.selectedGame {
            let availStr = ByteCountFormatter.string(fromByteCount: avail, countStyle: .file)
            HStack(spacing: 8) {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                    .font(.system(size: 14))
                Text("Storage ready: ~\(availStr) available on volume for \(sel.name) (\(sel.formattedSize))")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(NXTheme.textSecondary)
            }
            .padding(.horizontal, 4)
        }
    }
}
