import SwiftUI
import AppKit
import UniformTypeIdentifiers

// MARK: - Subviews and Card Helpers for GameSettingsSheetView
extension GameSettingsSheetView {
    
    func footerBar(onDismiss: @escaping () -> Void) -> some View {
        let footerContent = VStack(spacing: 10) {
            NXGlassContainer(spacing: 10) {
                HStack(spacing: 12) {
                    Button(viewModel.tr("personalization.reset_defaults")) {
                        viewModel.activeSettingsConfig = GameRuntimeConfig()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.regular)
                    .overlay(
                        Capsule()
                            .stroke(viewModel.settingsModalFocusIndex == 5 ? NXTheme.secondary : Color.clear, lineWidth: 2)
                    )
                    
                    Spacer()
                    
                    Button(viewModel.tr("common.cancel")) {
                        viewModel.gameForSettingsSheet = nil
                        onDismiss()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.regular)
                    .overlay(
                        Capsule()
                            .stroke(viewModel.settingsModalFocusIndex == 6 ? NXTheme.secondary : Color.clear, lineWidth: 2)
                    )
                    
                    Button(viewModel.tr("common.save")) {
                        viewModel.saveRuntimeConfig(viewModel.activeSettingsConfig, for: game)
                        viewModel.gameForSettingsSheet = nil
                        onDismiss()
                    }
                    .buttonStyle(NXPrimaryButtonStyle())
                    .controlSize(.regular)
                    .overlay(
                        Capsule()
                            .stroke(viewModel.settingsModalFocusIndex == 7 ? Color.white : Color.clear, lineWidth: 2)
                    )
                    .shadow(color: viewModel.settingsModalFocusIndex == 7 ? NXTheme.primary.opacity(0.8) : Color.clear, radius: 8)
                }
            }
            
            // Controller navigation hints strip
            HStack(spacing: 12) {
                hintBadge(glyph: "↑/↓", text: viewModel.tr("controls.hint_navigate"), isPill: true)
                hintBadge(glyph: "←/→", text: viewModel.tr("controls.hint_adjust"), isPill: true)
                hintBadge(
                    glyph: controllerService.activeBrand == .playstation ? "✕" : "A",
                    text: viewModel.tr("controls.hint_toggle"),
                    bgColor: Color(red: 0.06, green: 0.49, blue: 0.06)
                )
                hintBadge(
                    glyph: controllerService.activeBrand == .playstation ? "□" : "X",
                    text: viewModel.tr("settings_modal.save_and_close"),
                    bgColor: Color(red: 0.0, green: 0.47, blue: 0.84)
                )
                hintBadge(
                    glyph: controllerService.activeBrand == .playstation ? "○" : "B",
                    text: viewModel.tr("common.close"),
                    bgColor: Color(red: 0.91, green: 0.07, blue: 0.14)
                )
            }
            .padding(.top, 2)
        }
        
        return Group {
            if useLiquidGlass {
                footerContent
                    .padding(.horizontal, 16)
                    .padding(.vertical, 12)
                    .nxGlassBar(in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                    .padding(.horizontal, 12)
                    .padding(.top, 4)
                    .padding(.bottom, 10)
            } else {
                footerContent
                    .padding(.horizontal, 20)
                    .padding(.vertical, 14)
                    .background(
                        ZStack {
                            NXTheme.glassSurfaceElevated.opacity(0.85)
                            Rectangle().fill(.ultraThinMaterial)
                        }
                    )
                    .overlay(
                        Rectangle()
                            .frame(height: 1)
                            .foregroundColor(NXTheme.borderSubtle),
                        alignment: .top
                    )
            }
        }
    }
    
    func hintBadge(glyph: String, text: String, isPill: Bool = false, bgColor: Color = Color(white: 0.25)) -> some View {
        HStack(spacing: 4) {
            if isPill {
                Text(glyph)
                    .font(.system(size: 9, weight: .bold, design: .rounded))
                    .foregroundColor(.white)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(bgColor))
            } else {
                Text(glyph)
                    .font(.system(size: 9, weight: .black, design: .rounded))
                    .foregroundColor(.white)
                    .frame(width: 16, height: 16)
                    .background(Circle().fill(bgColor))
            }
            Text(text)
                .font(.system(size: 10, weight: .medium, design: .rounded))
                .foregroundColor(NXTheme.textSecondary)
        }
    }
    
    func settingCard<Content: View>(
        rowIndex: Int,
        title: String,
        icon: String,
        iconColor: Color,
        @ViewBuilder content: () -> Content
    ) -> some View {
        let isFocused = viewModel.settingsModalFocusIndex == rowIndex
        
        return VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Image(systemName: icon)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(iconColor)
                Text(title)
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                
                Spacer()
                
                if isFocused {
                    Text(viewModel.tr("settings_modal.focused"))
                        .font(.system(size: 9, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(NXTheme.primary.opacity(0.3))
                        .clipShape(Capsule())
                }
            }
            
            content()
        }
        .padding(14)
        .nxGlassCard(
            cornerRadius: 14,
            tint: isFocused ? NXTheme.primary.opacity(0.28) : nil
        )
        .overlay(
            Group {
                if isFocused {
                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                        .strokeBorder(NXTheme.primaryGradient, lineWidth: 2.2)
                }
            }
        )
        .shadow(color: isFocused ? NXTheme.primary.opacity(0.4) : Color.clear, radius: 8, x: 0, y: 2)
        .animation(.easeInOut(duration: 0.15), value: isFocused)
        .onTapGesture {
            viewModel.settingsModalFocusIndex = rowIndex
        }
    }
    
    func coverArtworkSection() -> some View {
        let isCustom = GameCoverService.shared.hasCustomCover(for: game)
        let currentCover = GameCoverService.shared.coverImage(for: game) ?? NSWorkspace.shared.icon(forFile: game.url.path)
        
        return settingCard(
            rowIndex: 5,
            title: viewModel.tr("settings_modal.cover_title"),
            icon: "photo.on.rectangle.angled",
            iconColor: Color(red: 0.95, green: 0.45, blue: 0.75)
        ) {
            HStack(spacing: 16) {
                Image(nsImage: currentCover)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(width: 55, height: 78)
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .stroke(NXTheme.specularRim, lineWidth: 1)
                    )
                    .shadow(color: Color.black.opacity(0.4), radius: 6, x: 0, y: 2)
                
                VStack(alignment: .leading, spacing: 5) {
                    HStack(spacing: 6) {
                        Text(isCustom ? viewModel.tr("settings_modal.cover_custom_active") : viewModel.tr("settings_modal.cover_auto_active"))
                            .font(.system(size: 12, weight: .bold, design: .rounded))
                            .foregroundColor(isCustom ? NXTheme.secondary : NXTheme.textPrimary)
                        
                        if isCustom {
                            Text(viewModel.tr("settings_modal.cover_custom_badge"))
                                .font(.system(size: 8, weight: .heavy, design: .monospaced))
                                .padding(.horizontal, 4)
                                .padding(.vertical, 1)
                                .background(NXTheme.secondary.opacity(0.25))
                                .foregroundColor(NXTheme.secondary)
                                .clipShape(Capsule())
                        }
                    }
                    
                    Text(isCustom
                         ? viewModel.tr("settings_modal.cover_custom_desc")
                         : viewModel.tr("settings_modal.cover_auto_desc"))
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(viewModel.settingsModalFocusIndex == 5 ? .white : .white.opacity(0.88))
                        .fixedSize(horizontal: false, vertical: true)
                    
                    NXGlassContainer(spacing: 8) {
                        HStack(spacing: 8) {
                            Button(viewModel.tr("settings_modal.cover_choose_btn")) {
                                let panel = NSOpenPanel()
                                panel.title = viewModel.tr("settings_modal.cover_panel_title")
                                panel.allowedContentTypes = [.jpeg, .png, .webP, .image]
                                panel.allowsMultipleSelection = false
                                panel.canChooseDirectories = false
                                if panel.runModal() == .OK, let url = panel.url {
                                    SoundEffectService.shared.play(.click)
                                    _ = GameCoverService.shared.setCustomCover(imageURL: url, for: game)
                                }
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            .controlSize(.small)
                            
                            if isCustom {
                                Button(viewModel.tr("settings_modal.cover_reset_btn")) {
                                    SoundEffectService.shared.play(.click)
                                    GameCoverService.shared.resetCustomCover(for: game)
                                }
                                .buttonStyle(NXSecondaryButtonStyle())
                                .controlSize(.small)
                                .foregroundColor(Color(red: 1.0, green: 0.4, blue: 0.4))
                            }
                        }
                    }
                    .padding(.top, 2)
                }
                Spacer()
            }
        }
    }
    
    func gameManagementSection() -> some View {
        GlassCard(padding: 14) {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Label(viewModel.tr("settings_modal.management_title"), systemImage: "gearshape.2.fill")
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    Spacer()
                }
                
                Text(viewModel.tr("settings_modal.management_desc"))
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(.white.opacity(0.88))
                
                HStack(spacing: 12) {
                    Spacer()
                    
                    NXGlassContainer(spacing: 12) {
                        HStack(spacing: 12) {
                            Button(action: {
                                dismissSettingsAndShowRemove()
                            }) {
                                HStack(spacing: 6) {
                                    Image(systemName: "eye.slash")
                                    Text(viewModel.tr("actions.remove_from_library"))
                                }
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            
                            Button(action: {
                                dismissSettingsAndShowTrash()
                            }) {
                                HStack(spacing: 6) {
                                    Image(systemName: "trash.fill")
                                    Text(viewModel.tr("actions.move_to_trash"))
                                }
                                .foregroundColor(Color(red: 0.95, green: 0.35, blue: 0.35))
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                        }
                    }
                }
                .padding(.top, 2)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxWidth: .infinity)
    }
}
