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
        guard let url = Bundle.main.url(forResource: currentImageName, withExtension: "png") ??
                        Bundle.main.url(forResource: "AppIcon", withExtension: "png"),
              let img = NSImage(contentsOf: url) else {
            return nil
        }
        let copy = img.copy() as! NSImage
        copy.size = NSSize(width: 30, height: 30)
        return copy
    }
    
    public var body: some View {
        Menu {
            Section("Icon Appearance") {
                ForEach(AppIconAppearanceMode.allCases, id: \.self) { mode in
                    Button(action: {
                        viewModel.iconAppearanceMode = mode
                    }) {
                        HStack {
                            Text(mode.rawValue)
                            if viewModel.iconAppearanceMode == mode {
                                Image(systemName: "checkmark")
                            }
                        }
                    }
                }
            }
        } label: {
            ZStack {
                if let img = currentNSImage {
                    Image(nsImage: img)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: 30, height: 30)
                        .clipShape(RoundedRectangle(cornerRadius: 7.5, style: .continuous))
                        .shadow(color: NXTheme.primary.opacity(isHovered ? 0.4 : 0.2), radius: isHovered ? 6 : 3, x: 0, y: 1)
                } else {
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: 20))
                        .foregroundColor(NXTheme.primary)
                        .frame(width: 30, height: 30)
                }
            }
            .frame(width: 30, height: 30)
            .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .frame(width: 30, height: 30)
        .clipped()
        .nxNoFocusRing()
        .focusable(false)
        .onHover { isHovered = $0 }
        .help("NxNative Logo: Adapts to Light, Dark & Mono modes. Click to choose appearance.")
        .onChange(of: colorScheme) { _ in
            if viewModel.iconAppearanceMode == .auto {
                viewModel.updateDockIcon()
            }
        }
    }
}
