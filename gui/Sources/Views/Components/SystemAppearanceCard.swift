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
        
        if let url = Bundle.main.url(forResource: name, withExtension: "png"),
           let img = NSImage(contentsOf: url) {
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
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Text(viewModel.tr("personalization.app_icon"))
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                    
                    Spacer()
                }
                
                HStack(spacing: 12) {
                    ForEach(AppIconAppearanceMode.allCases, id: \.self) { mode in
                        let isSelected: Bool = (viewModel.iconAppearanceMode == mode)
                        Button(action: {
                            withAnimation(.spring(response: 0.25, dampingFraction: 0.75)) {
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
                                                .strokeBorder(isSelected ? Color.cyan : Color.white.opacity(0.15), lineWidth: isSelected ? 2 : 1)
                                        )
                                        .shadow(color: isSelected ? Color.cyan.opacity(0.4) : Color.black.opacity(0.2), radius: isSelected ? 8 : 3, x: 0, y: 2)
                                    
                                    if isSelected {
                                        VStack {
                                            HStack {
                                                Spacer()
                                                Image(systemName: "checkmark.circle.fill")
                                                    .font(.system(size: 14, weight: .bold))
                                                    .foregroundColor(.white)
                                                    .background(Circle().fill(Color.cyan))
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
                            .background(
                                RoundedRectangle(cornerRadius: 10, style: .continuous)
                                    .fill(isSelected ? NXTheme.primary.opacity(0.25) : NXTheme.glassSurfaceElevated.opacity(0.6))
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: 10, style: .continuous)
                                    .strokeBorder(isSelected ? Color.cyan.opacity(0.6) : Color.white.opacity(0.18), lineWidth: 0.8)
                            )
                        }
                        .buttonStyle(.plain)
                        .nxNoFocusRing()
                    }
                }
            }
        }
    }
}
