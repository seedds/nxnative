import SwiftUI
import AppKit

public struct AppLogoView: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.colorScheme) private var colorScheme
    @State private var isHovered: Bool = false
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var currentImageName: String {
        switch viewModel.iconAppearanceMode {
        case .light:
            return "AppIcon-Light"
        case .dark:
            return "AppIcon-Dark"
        case .mono:
            return "AppIcon-Mono"
        case .auto:
            return colorScheme == .dark ? "AppIcon-Dark" : "AppIcon-Light"
        }
    }
    
    private var currentNSImage: NSImage? {
        AppIconCache.image(named: currentImageName)
    }
    
    public var body: some View {
        Button(action: {
            SoundEffectService.shared.play(.click)
            viewModel.selectedTab = 4 // Abre a aba Personalize
        }) {
            Group {
                if let img = currentNSImage {
                    Image(nsImage: img)
                        .resizable()
                        .interpolation(.high)
                        .antialiased(true)
                        .aspectRatio(1.0, contentMode: .fit)
                        .frame(width: 36, height: 36)
                        .shadow(color: NXTheme.primary.opacity(isHovered ? 0.45 : 0.2), radius: isHovered ? 6 : 3, x: 0, y: 1)
                } else {
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: 20))
                        .foregroundColor(NXTheme.primary)
                        .frame(width: 36, height: 36)
                }
            }
            .frame(width: 36, height: 36)
            .fixedSize()
            .scaleEffect(isHovered ? 1.04 : 1.0)
            .animation(.easeInOut(duration: 0.15), value: isHovered)
            .contentShape(Rectangle())
        }
        .buttonStyle(NXPressableButtonStyle())
        .fixedSize()
        .nxNoFocusRing()
        .onHover { isHovered = $0 }
        .contextMenu {
            Button(action: {
                SoundEffectService.shared.play(.click)
                viewModel.showAboutSheet = true
            }) {
                Label(viewModel.tr("applogo.about"), systemImage: "info.circle")
            }
            
            Button(action: {
                SoundEffectService.shared.play(.click)
                viewModel.openUpdateCheckerAndAutoInstall()
            }) {
                Label(viewModel.tr("system.check_updates"), systemImage: "arrow.triangle.2.circlepath")
            }
            
            Divider()
            
            Button(action: {
                SoundEffectService.shared.play(.tabSwitch)
                viewModel.selectedTab = 4
            }) {
                Label(viewModel.tr("applogo.personalize"), systemImage: "paintpalette.fill")
            }
        }
        .help(viewModel.tr("applogo.help"))
        .onChange(of: colorScheme) { _ in
            if viewModel.iconAppearanceMode == .auto {
                viewModel.updateDockIcon()
            }
        }
    }
}
