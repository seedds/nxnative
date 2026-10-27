import SwiftUI
import AppKit

public struct LibraryToolbarView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        HStack(spacing: 16) {
            // Search Field
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass")
                    .foregroundColor(NXTheme.textSecondary)
                TextField("Search ported games...", text: $viewModel.librarySearchText)
                    .textFieldStyle(.plain)
                    .foregroundColor(NXTheme.textPrimary)
                if !viewModel.librarySearchText.isEmpty {
                    Button(action: { viewModel.librarySearchText = "" }) {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundColor(NXTheme.textMuted)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
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
            
            // Console Card Scale Slider (0.70x to 1.35x continuous slider)
            HStack(spacing: 8) {
                Image(systemName: "minus.magnifyingglass")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundColor(NXTheme.textMuted)
                
                Slider(
                    value: $viewModel.libraryCardScale,
                    in: 0.70...1.35,
                    step: 0.05
                )
                .frame(width: 110)
                .accentColor(NXTheme.secondary)
                
                Image(systemName: "plus.magnifyingglass")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundColor(NXTheme.textMuted)
                
                Text("\(Int(viewModel.libraryCardScale * 100))%")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(NXTheme.textSecondary)
                    .frame(width: 38, alignment: .trailing)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(NXTheme.glassSurfaceElevated.opacity(0.6))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
            )
            
            Spacer()
            
            // Library Folder Indicator
            HStack(spacing: 6) {
                Image(systemName: "folder")
                    .foregroundColor(NXTheme.secondary)
                Text(viewModel.libraryFolder?.lastPathComponent ?? "Default Games")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(NXTheme.textSecondary)
                    .lineLimit(1)
                
                Button("Change...") {
                    SoundEffectService.shared.play(.click)
                    viewModel.selectLibraryFolder()
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
            }
            
            Button(action: {
                SoundEffectService.shared.play(.click)
                viewModel.refreshLibrary()
            }) {
                HStack(spacing: 4) {
                    if viewModel.isScanningLibrary {
                        ProgressView()
                            .controlSize(.small)
                    } else {
                        Image(systemName: "arrow.clockwise")
                    }
                    Text("Refresh")
                }
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .controlSize(.small)
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 10)
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
    }
}
