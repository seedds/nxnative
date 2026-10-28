import SwiftUI
import AppKit

public struct GameSettingsSheetView: View {
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject var controllerService = GameControllerService.shared
    public let game: PortedGame
    
    @Environment(\.dismiss) private var dismiss
    
    public init(viewModel: AppViewModel, game: PortedGame) {
        self.viewModel = viewModel
        self.game = game
    }
    
    public var body: some View {
        ZStack {
            NXTheme.canvasObsidian.ignoresSafeArea()
            
            VStack(spacing: 0) {
                // MARK: - Header
                HStack(spacing: 14) {
                    let icon = GameCoverService.shared.coverImage(for: game) ?? NSWorkspace.shared.icon(forFile: game.url.path)
                    Image(nsImage: icon)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: 52, height: 52)
                        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                        .shadow(color: NXTheme.primary.opacity(0.35), radius: 8, x: 0, y: 3)
                        .overlay(
                            RoundedRectangle(cornerRadius: 12, style: .continuous)
                                .stroke(NXTheme.specularRim, lineWidth: 1)
                        )
                    
                    VStack(alignment: .leading, spacing: 3) {
                        Text(game.name)
                            .font(.system(size: 16, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                            .lineLimit(1)
                        
                        HStack(spacing: 6) {
                            Text(game.platform == .macApp ? "Mac App" : "iOS IPA")
                                .font(.system(size: 10, weight: .bold))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(NXTheme.primary.opacity(0.2))
                                .foregroundColor(.white)
                                .clipShape(Capsule())
                            
                            if let ver = game.versionString {
                                Text("v\(ver)")
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundColor(NXTheme.textSecondary)
                            }
                            
                            if let tid = game.titleId {
                                Text("[\(tid)]")
                                    .font(.system(size: 10, design: .monospaced))
                                    .foregroundColor(NXTheme.textMuted)
                            }
                        }
                    }
                    
                    Spacer()
                    
                    Button(action: {
                        viewModel.gameForSettingsSheet = nil
                        dismiss()
                    }) {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 18))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    .buttonStyle(.plain)
                    .nxNoFocusRing()
                    .focusable(false)
                }
                .padding(18)
                .background(
                    ZStack {
                        NXTheme.glassSurfaceElevated.opacity(0.7)
                        Rectangle().fill(.ultraThinMaterial)
                    }
                )
                .overlay(
                    Rectangle()
                        .frame(height: 1)
                        .foregroundColor(NXTheme.borderSubtle),
                    alignment: .bottom
                )
                
                // MARK: - Settings Form
                ScrollView {
                    VStack(alignment: .leading, spacing: 16) {
                        if game.platform == .macApp {
                            // Section 0: Framerate (60 FPS)
                            settingCard(
                                rowIndex: 0,
                                title: viewModel.tr("settings_modal.framerate_title"),
                                icon: "speedometer",
                                iconColor: Color(red: 0.13, green: 0.77, blue: 0.37)
                            ) {
                                Toggle(isOn: $viewModel.activeSettingsConfig.enable60FPS) {
                                    VStack(alignment: .leading, spacing: 3) {
                                        Text(viewModel.activeSettingsConfig.enable60FPS ? viewModel.tr("settings_modal.fps_60") : viewModel.tr("settings_modal.fps_30"))
                                            .font(.system(size: 13, weight: .semibold, design: .rounded))
                                            .foregroundColor(NXTheme.textPrimary)
                                        Text(viewModel.tr("settings_modal.fps_desc"))
                                            .font(.system(size: 11))
                                            .foregroundColor(NXTheme.textSecondary)
                                    }
                                }
                                .toggleStyle(.switch)
                            }
                            
                            // Section 1: Metal HUD
                            settingCard(
                                rowIndex: 1,
                                title: viewModel.tr("settings_modal.metal_hud_title"),
                                icon: "gauge.with.needle.fill",
                                iconColor: NXTheme.secondary
                            ) {
                                Toggle(isOn: $viewModel.activeSettingsConfig.enableMetalHUD) {
                                    VStack(alignment: .leading, spacing: 3) {
                                        Text(viewModel.activeSettingsConfig.enableMetalHUD ? viewModel.tr("settings_modal.metal_hud_enabled") : viewModel.tr("settings_modal.metal_hud_disabled"))
                                            .font(.system(size: 13, weight: .semibold, design: .rounded))
                                            .foregroundColor(NXTheme.textPrimary)
                                        Text(viewModel.tr("settings_modal.metal_hud_desc"))
                                            .font(.system(size: 11))
                                            .foregroundColor(NXTheme.textSecondary)
                                    }
                                }
                                .toggleStyle(.switch)
                            }
                            
                            // Section 2: Console Mode
                            settingCard(
                                rowIndex: 2,
                                title: viewModel.tr("settings_modal.console_mode_title"),
                                icon: "tv.and.mediabox",
                                iconColor: NXTheme.primary
                            ) {
                                VStack(alignment: .leading, spacing: 8) {
                                    Picker("", selection: $viewModel.activeSettingsConfig.isDockedMode) {
                                        Text(viewModel.tr("settings_modal.docked_label")).tag(true)
                                        Text(viewModel.tr("settings_modal.handheld_label")).tag(false)
                                    }
                                    .pickerStyle(.segmented)
                                    
                                    Text(viewModel.activeSettingsConfig.isDockedMode
                                         ? viewModel.tr("settings_modal.docked_desc")
                                         : viewModel.tr("settings_modal.handheld_desc"))
                                        .font(.system(size: 11))
                                        .foregroundColor(NXTheme.textSecondary)
                                }
                            }
                            
                            // Section 3: 3D Render Scale
                            settingCard(
                                rowIndex: 3,
                                title: viewModel.tr("settings_modal.render_scale_title"),
                                icon: "viewfinder.circle.fill",
                                iconColor: Color(red: 1.0, green: 0.65, blue: 0.35)
                            ) {
                                VStack(alignment: .leading, spacing: 10) {
                                    HStack {
                                        Text(viewModel.tr("settings_modal.render_scale_label"))
                                            .font(.system(size: 12, weight: .medium))
                                            .foregroundColor(NXTheme.textSecondary)
                                        Spacer()
                                        Text("\(Int(round(viewModel.activeSettingsConfig.renderScale * 100)))%")
                                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                                            .foregroundColor(NXTheme.secondary)
                                    }
                                    
                                    Slider(value: $viewModel.activeSettingsConfig.renderScale, in: 0.5...1.0, step: 0.05)
                                        .tint(NXTheme.primary)
                                    
                                    HStack(spacing: 8) {
                                        Button(viewModel.tr("settings_modal.scale_50")) {
                                            viewModel.activeSettingsConfig.renderScale = 0.50
                                        }
                                        .buttonStyle(NXSecondaryButtonStyle())
                                        .controlSize(.mini)
                                        
                                        Button(viewModel.tr("settings_modal.scale_75")) {
                                            viewModel.activeSettingsConfig.renderScale = 0.75
                                        }
                                        .buttonStyle(NXSecondaryButtonStyle())
                                        .controlSize(.mini)
                                        
                                        Button(viewModel.tr("settings_modal.resolution_native")) {
                                            viewModel.activeSettingsConfig.renderScale = 1.00
                                        }
                                        .buttonStyle(NXSecondaryButtonStyle())
                                        .controlSize(.mini)
                                    }
                                }
                            }
                            
                            // Section 4: MetalFX Upscaling
                            settingCard(
                                rowIndex: 4,
                                title: viewModel.tr("settings_modal.metalfx_title"),
                                icon: "sparkles.tv.fill",
                                iconColor: NXTheme.secondary
                            ) {
                                Toggle(isOn: $viewModel.activeSettingsConfig.enableMetalFXUpscale) {
                                    VStack(alignment: .leading, spacing: 3) {
                                        Text(viewModel.activeSettingsConfig.enableMetalFXUpscale ? viewModel.tr("settings_modal.metalfx_enabled") : viewModel.tr("settings_modal.metalfx_disabled"))
                                            .font(.system(size: 13, weight: .semibold, design: .rounded))
                                            .foregroundColor(NXTheme.textPrimary)
                                        Text(viewModel.tr("settings_modal.metalfx_desc"))
                                            .font(.system(size: 11))
                                            .foregroundColor(NXTheme.textSecondary)
                                    }
                                }
                                .toggleStyle(.switch)
                            }
                            
                            // Section 5: Cover Artwork Customization
                            coverArtworkSection()
                            
                            // Section 6: Mods Management & Save Sync
                            GameSettingsModsAndSavesView(game: game)
                        } else {
                            // iOS Port: Installation, AirDrop, Sideload & Saves
                            GameSettingsModsAndSavesView(game: game)
                            
                            // Cover Artwork Customization
                            coverArtworkSection()
                        }
                        
                        // Section: Game Management (Remove / Trash)
                        gameManagementSection()
                    }
                    .padding(20)
                }
                
                // MARK: - Footer Actions & Gamepad Guidance
                footerBar(onDismiss: { dismiss() })
            }
        }
        .frame(minWidth: 500, idealWidth: 560, maxWidth: 640, minHeight: 560, idealHeight: 640, maxHeight: 740)
    }
    
    func dismissSettingsAndShowRemove() {
        let target = game
        viewModel.gameForSettingsSheet = nil
        dismiss()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
            viewModel.gameToRemove = target
            viewModel.showRemoveConfirm = true
        }
    }
    
    func dismissSettingsAndShowTrash() {
        let target = game
        viewModel.gameForSettingsSheet = nil
        dismiss()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
            viewModel.gameToTrash = target
            viewModel.showTrashConfirm = true
        }
    }
}

