import SwiftUI

public struct MainView: View {
    @ObservedObject public var viewModel: AppViewModel
    @State private var windowWidth: CGFloat = 1100
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private let tabs: [(tag: Int, title: String, icon: String)] = [
        (0, "Port Game", "play.circle.fill"),
        (1, "Library", "books.vertical.fill"),
        (2, "Compatibility", "checklist.checked"),
        (3, "Controls", "gamecontroller.fill"),
        (4, "Personalize", "paintpalette.fill"),
        (5, "Settings", "gearshape.fill"),
        (6, "Console", "terminal.fill")
    ]
    
    private var isCompactNav: Bool {
        windowWidth < 1080
    }
    
    public var body: some View {
        GeometryReader { windowGeo in
            ZStack {
                // MARK: - Obsidian Liquid Glass Canvas & Atmospheric Refractions
                Color.nxCanvas.ignoresSafeArea()
                
                // Atmospheric ambient glows
                ZStack {
                    Circle()
                        .fill(NXTheme.primary.opacity(0.15))
                        .frame(width: 450, height: 450)
                        .blur(radius: 90)
                        .position(x: 100, y: 40)
                    
                    Circle()
                        .fill(NXTheme.tertiary.opacity(0.07))
                        .frame(width: 400, height: 400)
                        .blur(radius: 100)
                        .position(x: windowGeo.size.width - 80, y: windowGeo.size.height - 60)
                }
                .ignoresSafeArea()
                
                VStack(spacing: 0) {
                    // MARK: - Liquid Glass Navigation Bar
                    navigationHeader
                    
                    // MARK: - Selected Content Surface
                    Group {
                        switch viewModel.selectedTab {
                        case 0:
                            BuildView(viewModel: viewModel)
                        case 1:
                            LibraryView(viewModel: viewModel)
                        case 2:
                            CompatibilityView(viewModel: viewModel)
                        case 3:
                            ControlsView()
                        case 4:
                            PersonalizationView(viewModel: viewModel)
                        case 5:
                            SystemView(viewModel: viewModel)
                        case 6:
                            LogsView(viewModel: viewModel)
                        default:
                            BuildView(viewModel: viewModel)
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    
                    // Persistent Controller Prompt Bar across tabs (when not handled inside tab 1)
                    if viewModel.selectedTab != 1 {
                        ControllerPromptBar(viewModel: viewModel)
                    }
                }
            }
            .onAppear {
                self.windowWidth = windowGeo.size.width
            }
            .onChange(of: windowGeo.size.width) { newWidth in
                self.windowWidth = newWidth
            }
        }
        .frame(minWidth: 880, minHeight: 620)
        .background(
            KeyboardNavBridge { key in
                viewModel.handleKeyboardKey(key)
            }
        )
        .sheet(isPresented: $viewModel.showAboutSheet) {
            AboutLauncherView(viewModel: viewModel)
        }
        .sheet(isPresented: $viewModel.showUpdateSheet) {
            LauncherUpdateSheet(viewModel: viewModel)
        }
    }
    
    private var navigationHeader: some View {
        HStack(spacing: 16) {
            // Brand Logo & Subtitle
            HStack(spacing: 10) {
                AppLogoView(viewModel: viewModel)
                
                VStack(alignment: .leading, spacing: 1) {
                    HStack(spacing: 6) {
                        Text("NxNative Launcher")
                            .font(.system(size: 15, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        
                        if let update = viewModel.updateInfo, update.hasUpdate {
                            Button(action: {
                                SoundEffectService.shared.play(.click)
                                viewModel.openUpdateCheckerAndAutoInstall()
                            }) {
                                HStack(spacing: 4) {
                                    Circle()
                                        .fill(NXTheme.secondary)
                                        .frame(width: 5, height: 5)
                                    Text("Update")
                                        .font(.system(size: 9, weight: .bold))
                                        .foregroundColor(.white)
                                }
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(NXTheme.secondary.opacity(0.35))
                                .clipShape(Capsule())
                            }
                            .buttonStyle(.plain)
                            .help("Click to install the latest NxNative Launcher update from GitHub")
                        }
                    }
                }
            }
            
            Spacer()
            
            // Epic Liquid Glass Navigation Tabs (Responsive: Icons only when window is small)
            HStack(spacing: isCompactNav ? 3 : 4) {
                ForEach(tabs, id: \.tag) { item in
                    let isSelected = viewModel.selectedTab == item.tag
                    Button(action: {
                        SoundEffectService.shared.play(.tabSwitch)
                        viewModel.selectedTab = item.tag
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: item.icon)
                                .font(.system(size: isCompactNav ? 13 : 12, weight: isSelected ? .bold : .semibold))
                            
                            if !isCompactNav {
                                Text(item.title)
                                    .font(.system(size: 12, weight: isSelected ? .bold : .semibold, design: .rounded))
                            }
                        }
                        .foregroundColor(.white)
                        .padding(.horizontal, isCompactNav ? 9 : 11)
                        .padding(.vertical, 6)
                        .background(
                            ZStack {
                                if isSelected {
                                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                                        .fill(NXTheme.primaryGradient)
                                        .overlay(
                                            RoundedRectangle(cornerRadius: 8, style: .continuous)
                                                .strokeBorder(Color.white.opacity(0.25), lineWidth: 0.8)
                                        )
                                        .shadow(color: NXTheme.primary.opacity(0.5), radius: 6, x: 0, y: 2)
                                } else {
                                    Color.clear
                                }
                            }
                        )
                        .animation(.spring(response: 0.22, dampingFraction: 0.78), value: isSelected)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .help(item.title)
                    .nxNoFocusRing()
                }
            }
            .padding(4)
            .background(
                ZStack {
                    RoundedRectangle(cornerRadius: 11, style: .continuous)
                        .fill(NXTheme.glassSurfaceElevated.opacity(0.7))
                    RoundedRectangle(cornerRadius: 11, style: .continuous)
                        .fill(.ultraThinMaterial)
                }
            )
            .overlay(
                RoundedRectangle(cornerRadius: 11, style: .continuous)
                    .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
            )
            
            Spacer()
            
            // Build & Readiness Status Indicator
            HStack(spacing: 8) {
                if viewModel.isBuilding {
                    HStack(spacing: 6) {
                        ProgressView()
                            .controlSize(.small)
                        Text("Building...")
                            .font(.system(size: 11, weight: .bold, design: .rounded))
                            .foregroundColor(.white)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 5)
                    .background(
                        Capsule()
                            .fill(NXTheme.primary.opacity(0.35))
                    )
                    .overlay(
                        Capsule()
                            .strokeBorder(NXTheme.secondary.opacity(0.6), lineWidth: 0.8)
                    )
                } else {
                    StatusBadge(
                        isPositive: viewModel.isProdKeysValid && viewModel.isNxnativeValid,
                        text: (viewModel.isProdKeysValid && viewModel.isNxnativeValid) ? "Environment Ready" : "Setup Required (Check Settings)",
                        showText: false
                    )
                }
            }
            .fixedSize(horizontal: true, vertical: false)
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 10)
        .frame(height: 54)
        .background(
            ZStack {
                NXTheme.canvasObsidian.opacity(0.85)
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
