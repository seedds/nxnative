import SwiftUI
import AppKit

public struct SystemAppearanceCard: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.colorScheme) private var colorScheme
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private func iconPreview(for mode: AppIconAppearanceMode) -> Image {
        let name: String
        switch mode {
        case .auto:
            name = colorScheme == .dark ? "AppIcon-Dark" : "AppIcon-Light"
        case .light:
            name = "AppIcon-Light"
        case .dark:
            name = "AppIcon-Dark"
        case .mono:
            name = "AppIcon-Mono"
        }
        
        if let img = AppIconCache.image(named: name) {
            return Image(nsImage: img)
        }
        return Image(systemName: "gamecontroller.fill")
    }
    
    private func modeTitle(for mode: AppIconAppearanceMode) -> String {
        switch mode {
        case .light:
            return viewModel.tr("personalization.icon_light")
        case .dark:
            return viewModel.tr("personalization.icon_dark")
        case .mono:
            return viewModel.tr("personalization.icon_mono")
        case .auto:
            return viewModel.tr("personalization.icon_auto")
        }
    }
    
    private func menuBarIconPreview(for style: MenuBarIconStyle) -> Image {
        let nsImg = MenuBarManager.shared.createMenuBarIcon(style: style)
        return Image(nsImage: nsImg)
    }

    private func menuBarModeTitle(for style: MenuBarIconStyle) -> String {
        switch style {
        case .symbol:
            return viewModel.tr("menubar.icon_style_symbol")
        case .dark:
            return viewModel.tr("menubar.icon_style_dark")
        case .lightColor:
            return viewModel.tr("menubar.icon_style_light_color")
        }
    }
    
    public var body: some View {
        GlassCard(cornerRadius: 16, padding: 18) {
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 8) {
                    Image(systemName: "app.badge.sparkles.fill")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(viewModel.highlightTheme.primaryColor)
                    
                    Text(viewModel.tr("personalization.app_icon"))
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                    
                    Spacer()
                }
                
                NXGlassContainer(spacing: 12) {
                    HStack(spacing: 12) {
                        ForEach(AppIconAppearanceMode.allCases, id: \.self) { mode in
                            let isSelected: Bool = (viewModel.iconAppearanceMode == mode)
                            Button(action: {
                                SoundEffectService.shared.play(.click)
                                withAnimation(.spring(response: 0.28, dampingFraction: 0.78)) {
                                    viewModel.iconAppearanceMode = mode
                                }
                            }) {
                                VStack(spacing: 8) {
                                    ZStack {
                                        iconPreview(for: mode)
                                            .resizable()
                                            .aspectRatio(contentMode: .fit)
                                            .frame(width: 44, height: 44)
                                            .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                                            .overlay(
                                                RoundedRectangle(cornerRadius: 10, style: .continuous)
                                                    .strokeBorder(
                                                        isSelected ? viewModel.highlightTheme.secondaryColor : Color.white.opacity(0.15),
                                                        lineWidth: isSelected ? 2 : 1
                                                    )
                                            )
                                            .shadow(
                                                color: isSelected ? viewModel.highlightTheme.primaryColor.opacity(0.45) : Color.black.opacity(0.2),
                                                radius: isSelected ? 8 : 3,
                                                x: 0,
                                                y: 2
                                            )
                                        
                                        if isSelected {
                                            VStack {
                                                HStack {
                                                    Spacer()
                                                    Image(systemName: "checkmark.circle.fill")
                                                        .font(.system(size: 14, weight: .bold))
                                                        .foregroundColor(.white)
                                                        .background(Circle().fill(viewModel.highlightTheme.primaryColor))
                                                        .offset(x: 4, y: -4)
                                                }
                                                Spacer()
                                            }
                                            .frame(width: 44, height: 44)
                                        }
                                    }
                                    
                                    Text(modeTitle(for: mode))
                                        .font(.system(size: 11, weight: isSelected ? .bold : .semibold, design: .rounded))
                                        .foregroundColor(.white)
                                }
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 10)
                                .padding(.horizontal, 8)
                                .contentShape(Rectangle())
                                .nxGlassInteractive(
                                    in: RoundedRectangle(cornerRadius: 12, style: .continuous),
                                    isSelected: isSelected,
                                    fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.6),
                                    fallbackSelectedFill: NXTheme.primary.opacity(0.25)
                                )
                            }
                            .buttonStyle(NXPressableButtonStyle())
                            .nxNoFocusRing()
                        }
                    }
                }
                
                Divider()
                    .background(Color.white.opacity(0.1))
                    .padding(.vertical, 4)

                HStack(spacing: 8) {
                    Image(systemName: "menubar.arrow.up.rectangle")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(viewModel.highlightTheme.primaryColor)
                    
                    Text(viewModel.tr("menubar.icon_style"))
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                    
                    Spacer()
                }

                NXGlassContainer(spacing: 12) {
                    HStack(spacing: 12) {
                        ForEach(MenuBarIconStyle.allCases, id: \.self) { style in
                            let isSelected: Bool = (viewModel.menuBarIconStyle == style)
                            Button(action: {
                                SoundEffectService.shared.play(.click)
                                withAnimation(.spring(response: 0.28, dampingFraction: 0.78)) {
                                    viewModel.menuBarIconStyle = style
                                }
                            }) {
                                VStack(spacing: 8) {
                                    ZStack {
                                        menuBarIconPreview(for: style)
                                            .resizable()
                                            .aspectRatio(contentMode: .fit)
                                            .frame(width: 28, height: 28)
                                            .padding(8)
                                            .background(
                                                RoundedRectangle(cornerRadius: 10, style: .continuous)
                                                    .fill(Color.black.opacity(0.4))
                                            )
                                            .overlay(
                                                RoundedRectangle(cornerRadius: 10, style: .continuous)
                                                    .strokeBorder(
                                                        isSelected ? viewModel.highlightTheme.secondaryColor : Color.white.opacity(0.15),
                                                        lineWidth: isSelected ? 2 : 1
                                                    )
                                            )
                                            .shadow(
                                                color: isSelected ? viewModel.highlightTheme.primaryColor.opacity(0.45) : Color.black.opacity(0.2),
                                                radius: isSelected ? 8 : 3,
                                                x: 0,
                                                y: 2
                                            )
                                        
                                        if isSelected {
                                            VStack {
                                                HStack {
                                                    Spacer()
                                                    Image(systemName: "checkmark.circle.fill")
                                                        .font(.system(size: 14, weight: .bold))
                                                        .foregroundColor(.white)
                                                        .background(Circle().fill(viewModel.highlightTheme.primaryColor))
                                                        .offset(x: 4, y: -4)
                                                }
                                                Spacer()
                                            }
                                            .frame(width: 44, height: 44)
                                        }
                                    }
                                    
                                    Text(menuBarModeTitle(for: style))
                                        .font(.system(size: 11, weight: isSelected ? .bold : .semibold, design: .rounded))
                                        .foregroundColor(.white)
                                }
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 10)
                                .padding(.horizontal, 8)
                                .contentShape(Rectangle())
                                .nxGlassInteractive(
                                    in: RoundedRectangle(cornerRadius: 12, style: .continuous),
                                    isSelected: isSelected,
                                    fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.6),
                                    fallbackSelectedFill: NXTheme.primary.opacity(0.25)
                                )
                            }
                            .buttonStyle(NXPressableButtonStyle())
                            .nxNoFocusRing()
                        }
                    }
                }
            }
        }
    }
}
