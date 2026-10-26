import SwiftUI
import AppKit

public struct PortedGameCard: View {
    public let game: PortedGame
    @ObservedObject public var viewModel: AppViewModel
    public let onDelete: (PortedGame) -> Void
    
    public init(game: PortedGame, viewModel: AppViewModel, onDelete: @escaping (PortedGame) -> Void) {
        self.game = game
        self.viewModel = viewModel
        self.onDelete = onDelete
    }
    
    public var body: some View {
        let config = viewModel.loadRuntimeConfig(for: game)
        
        return GlassCard(padding: 16) {
            VStack(spacing: 12) {
                // Top Platform Pill & Format
                HStack {
                    Text(game.platform == .macApp ? "Mac App" : "iOS IPA")
                        .font(.system(size: 10, weight: .bold, design: .rounded))
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(game.platform == .macApp ? NXTheme.primary.opacity(0.35) : NXTheme.secondary.opacity(0.35))
                        .foregroundColor(.white)
                        .clipShape(Capsule())
                    
                    Spacer()
                    
                    // Quick Setting Pills (Current Config - White text for high contrast)
                    HStack(spacing: 4) {
                        Text(config.enable60FPS ? "60 FPS" : "30 FPS")
                            .font(.system(size: 9, weight: .bold, design: .monospaced))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(config.enable60FPS ? Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.35) : NXTheme.textMuted.opacity(0.3))
                            .foregroundColor(.white)
                            .clipShape(Capsule())
                        
                        Text(config.isDockedMode ? "Docked" : "Handheld")
                            .font(.system(size: 9, weight: .bold, design: .monospaced))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(NXTheme.primary.opacity(0.35))
                            .foregroundColor(.white)
                            .clipShape(Capsule())
                        
                        if config.enableMetalFXUpscale {
                            Text("MetalFX")
                                .font(.system(size: 9, weight: .bold, design: .monospaced))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(NXTheme.secondary.opacity(0.35))
                                .foregroundColor(.white)
                                .clipShape(Capsule())
                        }
                    }
                }
                
                // Centered Large Artwork / Icon
                VStack(spacing: 8) {
                    let icon = NSWorkspace.shared.icon(forFile: game.url.path)
                    Image(nsImage: icon)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: 96, height: 96)
                        .clipShape(RoundedRectangle(cornerRadius: 20, style: .continuous))
                        .shadow(color: NXTheme.primary.opacity(0.3), radius: 10, x: 0, y: 4)
                        .overlay(
                            RoundedRectangle(cornerRadius: 20, style: .continuous)
                                .stroke(NXTheme.specularRim, lineWidth: 1.2)
                        )
                    
                    // Centered Name & Size (Date removed per user request)
                    VStack(spacing: 3) {
                        Text(game.name)
                            .font(.system(size: 14, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                            .multilineTextAlignment(.center)
                            .lineLimit(2)
                            .frame(maxWidth: .infinity)
                        
                        Text(game.formattedSize)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                }
                .padding(.vertical, 4)
                
                Divider()
                    .opacity(0.2)
                
                // Action Buttons Row (Play, Settings, Folder, More)
                HStack(spacing: 8) {
                    if game.isPlayableLocally {
                        Button(action: {
                            viewModel.launchGame(game)
                        }) {
                            HStack(spacing: 5) {
                                Image(systemName: "play.fill")
                                Text("Play")
                            }
                            .font(.system(size: 11, weight: .bold, design: .rounded))
                            .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                        .controlSize(.regular)
                    }
                    
                    Button(action: {
                        viewModel.gameForSettingsSheet = game
                    }) {
                        Image(systemName: "slider.horizontal.3")
                            .font(.system(size: 12, weight: .medium))
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.regular)
                    .help("Game Settings (60 FPS, Metal HUD, Docked, MetalFX)")
                    
                    Button(action: {
                        viewModel.revealInFinder(url: game.url)
                    }) {
                        Image(systemName: "folder")
                            .font(.system(size: 12))
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.regular)
                    .help("Reveal in Finder")
                    
                    // Context Menu with hidden chevron indicator to eliminate overlap
                    Menu {
                        Button(action: {
                            viewModel.gameForSettingsSheet = game
                        }) {
                            Label("Game Settings...", systemImage: "slider.horizontal.3")
                        }
                        Button("Copy Path") {
                            let pb = NSPasteboard.general
                            pb.clearContents()
                            pb.setString(game.url.path, forType: .string)
                        }
                        Divider()
                        Button("Delete Port...", role: .destructive) {
                            onDelete(game)
                        }
                    } label: {
                        Image(systemName: "ellipsis")
                            .font(.system(size: 12, weight: .bold))
                            .foregroundColor(NXTheme.textPrimary)
                            .frame(width: 28, height: 28)
                            .background(NXTheme.glassSurfaceElevated)
                            .cornerRadius(6)
                            .overlay(
                                RoundedRectangle(cornerRadius: 6)
                                    .stroke(NXTheme.specularRim, lineWidth: 0.8)
                            )
                    }
                    .menuStyle(.borderlessButton)
                    .menuIndicator(.hidden)
                    .fixedSize()
                    .help("More Options")
                    .nxNoFocusRing()
                }
            }
        }
    }
}
