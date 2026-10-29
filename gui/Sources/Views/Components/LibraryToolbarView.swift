import SwiftUI
import AppKit

public struct LibraryToolbarView: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var useFloatingGlass: Bool {
        glassEnv.isSupportedAndEnabled && !reduceTransparency
    }
    
    public var body: some View {
        NXGlassContainer(spacing: 12) {
            HStack(spacing: 12) {
                // Search Field Capsule (Crisp White Placeholder)
                NXLiquidGlassSearchField(
                    viewModel.tr("library.search_placeholder"),
                    text: $viewModel.librarySearchText
                )
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .frame(maxWidth: 240)
                .nxGlassBar(
                    in: Capsule(),
                    fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.85),
                    fallbackStroke: NXTheme.borderSubtle
                )
                
                // Console Card Scale Slider Capsule (0.70x to 1.35x Liquid Glass Slider)
                HStack(spacing: 8) {
                    Image(systemName: "minus.magnifyingglass")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundColor(.white.opacity(0.88))
                    
                    NXLiquidGlassSlider(
                        value: $viewModel.libraryCardScale,
                        in: 0.70...1.35,
                        step: 0.05,
                        accentColor: NXTheme.primary
                    )
                    .frame(width: 118)
                    
                    Image(systemName: "plus.magnifyingglass")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundColor(.white.opacity(0.88))
                    
                    Text("\(Int(round(viewModel.libraryCardScale * 100)))%")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(.white)
                        .frame(width: 38, alignment: .trailing)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 4)
                .nxGlassBar(
                    in: Capsule(),
                    fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.75),
                    fallbackStroke: NXTheme.borderSubtle
                )
                
                Spacer()
                
                // Library Folder Indicator & Actions
                HStack(spacing: 8) {
                    HStack(spacing: 6) {
                        Image(systemName: "folder.fill")
                            .font(.system(size: 11))
                            .foregroundColor(NXTheme.secondary)
                        Text(viewModel.libraryFolder?.lastPathComponent ?? viewModel.tr("library.default_games"))
                            .font(.system(size: 11, weight: .medium, design: .monospaced))
                            .foregroundColor(NXTheme.textPrimary.opacity(0.9))
                            .lineLimit(1)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .nxGlassBar(
                        in: Capsule(),
                        fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.65),
                        fallbackStroke: NXTheme.borderSubtle
                    )
                    
                    Button(viewModel.tr("library.change_folder")) {
                        SoundEffectService.shared.play(.click)
                        viewModel.selectLibraryFolder()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.small)
                    
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
                            Text(viewModel.tr("library.refresh"))
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.small)
                }
            }
            .padding(.horizontal, useFloatingGlass ? 16 : 18)
            .padding(.vertical, useFloatingGlass ? 8 : 10)
            .background(
                Group {
                    if !useFloatingGlass {
                        ZStack {
                            NXTheme.canvasObsidian.opacity(0.6)
                            Rectangle().fill(.ultraThinMaterial)
                        }
                    }
                }
            )
            .overlay(
                Group {
                    if !useFloatingGlass {
                        Rectangle()
                            .frame(height: 1)
                            .foregroundColor(NXTheme.borderSubtle)
                    }
                },
                alignment: .bottom
            )
        }
    }
}
