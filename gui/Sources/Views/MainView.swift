import SwiftUI

public struct MainView: View {
    @ObservedObject public var viewModel: AppViewModel
    @State private var windowWidth: CGFloat = 1100
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var tabs: [(tag: Int, title: String, icon: String)] {
        [
            (0, viewModel.tr("nav.port_game"), "play.circle.fill"),
            (1, viewModel.tr("nav.library"), "books.vertical.fill"),
            (2, viewModel.tr("nav.compatibility"), "checklist.checked"),
            (3, viewModel.tr("nav.controls"), "gamecontroller.fill"),
            (4, viewModel.tr("nav.personalize"), "paintpalette.fill"),
            (5, viewModel.tr("nav.settings"), "gearshape.fill"),
            (6, viewModel.tr("nav.console"), "terminal.fill")
        ]
    }
    
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
                        .zIndex(500)
                    
                    // MARK: - Selected Content Surface (Instant Tab Switching via Preserved ZStack)
                    ZStack {
                        BuildView(viewModel: viewModel)
                            .opacity(viewModel.selectedTab == 0 ? 1 : 0)
                            .allowsHitTesting(viewModel.selectedTab == 0)
                        
                        LibraryView(viewModel: viewModel)
                            .opacity(viewModel.selectedTab == 1 ? 1 : 0)
                            .allowsHitTesting(viewModel.selectedTab == 1)
                        
                        CompatibilityView(viewModel: viewModel)
                            .opacity(viewModel.selectedTab == 2 ? 1 : 0)
                            .allowsHitTesting(viewModel.selectedTab == 2)
                        
                        ControlsView()
                            .opacity(viewModel.selectedTab == 3 ? 1 : 0)
                            .allowsHitTesting(viewModel.selectedTab == 3)
                        
                        PersonalizationView(viewModel: viewModel)
                            .opacity(viewModel.selectedTab == 4 ? 1 : 0)
                            .allowsHitTesting(viewModel.selectedTab == 4)
                        
                        SystemView(viewModel: viewModel)
                            .opacity(viewModel.selectedTab == 5 ? 1 : 0)
                            .allowsHitTesting(viewModel.selectedTab == 5)
                        
                        LogsView(viewModel: viewModel)
                            .opacity(viewModel.selectedTab == 6 ? 1 : 0)
                            .allowsHitTesting(viewModel.selectedTab == 6)
                    }
                    .animation(.easeInOut(duration: 0.12), value: viewModel.selectedTab)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    
                    // Persistent Controller Prompt Bar across all tabs
                    ControllerPromptBar(viewModel: viewModel)
                        .zIndex(500)
                }
                .id(viewModel.launcherLanguage)
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
                                    Text(viewModel.tr("nav.update"))
                                        .font(.system(size: 9, weight: .bold))
                                        .foregroundColor(.white)
                                }
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(NXTheme.secondary.opacity(0.35))
                                .clipShape(Capsule())
                            }
                            .buttonStyle(.plain)
                            .help(viewModel.tr("nav.update_tooltip"))
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
                        guard !viewModel.isAnyModalPresented else { return }
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
                                    Color.white.opacity(0.001)
                                }
                            }
                        )
                        .animation(.interactiveSpring(response: 0.24, dampingFraction: 0.8), value: isSelected)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .disabled(viewModel.isAnyModalPresented)
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
                        Text(viewModel.tr("status.building"))
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
                        text: (viewModel.isProdKeysValid && viewModel.isNxnativeValid) ? viewModel.tr("status.env_ready") : viewModel.tr("status.setup_required"),
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
