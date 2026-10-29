import SwiftUI

public struct MainView: View {
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var menuBarManager = MenuBarManager.shared
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @State private var windowWidth: CGFloat = 1100
    @State private var animatedTabPill: Int = 0
    @Namespace private var navTabNamespace
    
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
        windowWidth < 1100
    }
    
    private var useFloatingGlassBars: Bool {
        if #available(macOS 26.0, *) {
            return viewModel.isLiquidGlassEnabled && !reduceTransparency
        }
        return false
    }
    
    public var body: some View {
        Group {
            if menuBarManager.isMinimizedToMenuBar {
                Color.clear
                    .frame(width: 1, height: 1)
            } else {
                GeometryReader { windowGeo in
                    ZStack {
                        // MARK: - Obsidian Liquid Glass Canvas & Atmospheric Refractions
                        Color.nxCanvas.ignoresSafeArea()
                        
                        // Dynamic ambient glows calibrated continuously to Liquid vs Frosted vs Classic modes
                        atmosphericGlows(in: windowGeo.size)
                        
                        // MARK: - Persistent Top Bar, Inner Tab Content, & Persistent Bottom Bar
                        VStack(spacing: 0) {
                            navigationHeader
                                .zIndex(10)
                            
                            ZStack {
                                selectedTabContent
                            }
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                            .clipped()
                            
                            ControllerPromptBar(viewModel: viewModel)
                                .zIndex(10)
                        }
                        .id(viewModel.launcherLanguage)
                    }
                    .onAppear {
                        self.windowWidth = windowGeo.size.width
                        self.animatedTabPill = viewModel.selectedTab
                    }
                    .onChange(of: windowGeo.size.width) { newWidth in
                        self.windowWidth = newWidth
                    }
                    .onChange(of: viewModel.selectedTab) { newTab in
                        withAnimation(.spring(response: 0.34, dampingFraction: 0.76, blendDuration: 0.10)) {
                            self.animatedTabPill = newTab
                        }
                    }
                }
            }
        }
        .frame(minWidth: 880, minHeight: 620)
        .environment(\.nxLiquidGlass, viewModel.liquidGlassEnvironment)
        .preferredColorScheme(.dark)
        .background(
            KeyboardNavBridge { key in
                viewModel.handleKeyboardKey(key)
            }
        )
        .sheet(isPresented: $viewModel.showAboutSheet) {
            AboutLauncherView(viewModel: viewModel)
                .environment(\.nxLiquidGlass, viewModel.liquidGlassEnvironment)
        }
        .sheet(isPresented: $viewModel.showUpdateSheet) {
            LauncherUpdateSheet(viewModel: viewModel)
                .environment(\.nxLiquidGlass, viewModel.liquidGlassEnvironment)
        }
    }
    
    @ViewBuilder
    private func atmosphericGlows(in size: CGSize) -> some View {
        let env = viewModel.liquidGlassEnvironment
        let primaryOpacity: Double = {
            if !viewModel.isLiquidGlassEnabled { return 0.14 }
            return env.mix(frosted: 0.17, liquid: 0.26)
        }()
        let secondaryOpacity: Double = {
            if !viewModel.isLiquidGlassEnabled { return 0.06 }
            return env.mix(frosted: 0.09, liquid: 0.16)
        }()
        let tertiaryOpacity: Double = {
            if !viewModel.isLiquidGlassEnabled { return 0.07 }
            return env.mix(frosted: 0.07, liquid: 0.12)
        }()
        
        ZStack {
            Circle()
                .fill(NXTheme.primary.opacity(primaryOpacity))
                .frame(width: 480, height: 480)
                .blur(radius: 95)
                .position(x: 120, y: 50)
            
            Circle()
                .fill(NXTheme.secondary.opacity(secondaryOpacity))
                .frame(width: 420, height: 420)
                .blur(radius: 110)
                .position(x: size.width * 0.52, y: 90)
            
            Circle()
                .fill(NXTheme.tertiary.opacity(tertiaryOpacity))
                .frame(width: 420, height: 420)
                .blur(radius: 105)
                .position(x: size.width - 80, y: size.height - 60)
        }
        .animation(.easeInOut(duration: 0.35), value: viewModel.highlightTheme)
        .animation(.easeInOut(duration: 0.30), value: viewModel.isLiquidGlassEnabled)
        .allowsHitTesting(false)
        .ignoresSafeArea()
    }
    
    @ViewBuilder
    private var selectedTabContent: some View {
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
            EmptyView()
        }
    }
    
    private var navigationHeader: some View {
        NXGlassContainer(spacing: 14) {
            HStack(spacing: 12) {
                // Left Island: Brand Logo & Update Pill
                brandIsland
                
                Spacer(minLength: 8)
                
                // Center Island: 7 Morphing Liquid Glass Navigation Tabs
                navigationTabsIsland
                
                Spacer(minLength: 8)
                
                // Right Island: Build & Readiness Status Indicator
                statusIsland
            }
            .padding(.horizontal, useFloatingGlassBars ? 14 : 18)
            .padding(.vertical, useFloatingGlassBars ? 8 : 10)
            .frame(minHeight: 54)
            .background(
                Group {
                    if !useFloatingGlassBars {
                        ZStack {
                            NXTheme.canvasObsidian.opacity(0.85)
                            Rectangle().fill(.ultraThinMaterial)
                        }
                    }
                }
            )
            .overlay(
                Group {
                    if !useFloatingGlassBars {
                        Rectangle()
                            .frame(height: 1)
                            .foregroundColor(NXTheme.borderSubtle)
                    }
                },
                alignment: .bottom
            )
        }
    }
    
    private var brandIsland: some View {
        HStack(spacing: 10) {
            AppLogoView(viewModel: viewModel)
            
            HStack(spacing: 6) {
                Text("NxNative Launcher")
                    .font(.system(size: 14, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                    .lineLimit(1)
                
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
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .nxGlassInteractive(
                            in: Capsule(),
                            tint: NXTheme.secondary.opacity(0.65),
                            isSelected: true,
                            fallbackFill: NXTheme.secondary.opacity(0.35),
                            fallbackSelectedFill: NXTheme.secondary.opacity(0.35)
                        )
                        .nxGlassTransition(.materialize)
                    }
                    .buttonStyle(NXPressableButtonStyle())
                    .help(viewModel.tr("nav.update_tooltip"))
                }
            }
        }
        .padding(.leading, useFloatingGlassBars ? 4 : 0)
        .padding(.trailing, useFloatingGlassBars ? 12 : 0)
        .padding(.vertical, useFloatingGlassBars ? 3 : 0)
        .modifier(BrandIslandCapsuleModifier(isEnabled: useFloatingGlassBars))
    }
    
    private var navigationTabsIsland: some View {
        NXGlassContainer(spacing: 6) {
            HStack(spacing: isCompactNav ? 3 : 4) {
                ForEach(tabs, id: \.tag) { item in
                    let isSelected = viewModel.selectedTab == item.tag
                    Button(action: {
                        guard !viewModel.isAnyModalPresented else { return }
                        guard viewModel.selectedTab != item.tag else { return }
                        SoundEffectService.shared.play(.tabSwitch)
                        viewModel.selectedTab = item.tag
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: item.icon)
                                .font(.system(size: isCompactNav ? 13 : 12, weight: isSelected ? .bold : .semibold))
                            
                            if !isCompactNav {
                                Text(item.title)
                                    .font(.system(size: 12, weight: isSelected ? .bold : .semibold, design: .rounded))
                                    .lineLimit(1)
                            }
                        }
                        .foregroundColor(isSelected ? .white : .white.opacity(0.86))
                        .padding(.horizontal, isCompactNav ? 10 : 12)
                        .padding(.vertical, 6.5)
                        .contentShape(Capsule())
                    }
                    .buttonStyle(NXPressableButtonStyle())
                    .disabled(viewModel.isAnyModalPresented)
                    .help(item.title)
                    .nxNoFocusRing()
                    .matchedGeometryEffect(id: item.tag, in: navTabNamespace, isSource: true)
                }
            }
            .background {
                NXLiquidSlidingPill(
                    in: Capsule(),
                    tint: NXTheme.primary.opacity(viewModel.liquidGlassEnvironment.mix(frosted: 0.86, liquid: 0.72))
                )
                .matchedGeometryEffect(
                    id: animatedTabPill,
                    in: navTabNamespace,
                    properties: .frame,
                    anchor: .center,
                    isSource: false
                )
            }
            .padding(4)
            .nxGlassBar(
                in: Capsule(),
                fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.75),
                fallbackStroke: NXTheme.borderSubtle
            )
        }
    }
    
    private var statusIsland: some View {
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
                .padding(.vertical, 6)
                .nxGlassBar(
                    in: Capsule(),
                    tint: NXTheme.primary.opacity(0.50),
                    fallbackFill: NXTheme.primary.opacity(0.35),
                    fallbackStroke: NXTheme.secondary.opacity(0.6)
                )
                .nxGlassTransition(.materialize)
            } else {
                StatusBadge(
                    isPositive: viewModel.isProdKeysValid && viewModel.isNxnativeValid,
                    text: (viewModel.isProdKeysValid && viewModel.isNxnativeValid)
                        ? viewModel.tr("status.env_ready")
                        : viewModel.tr("status.setup_required"),
                    showText: false
                )
                .padding(useFloatingGlassBars ? 7 : 0)
                .modifier(BrandIslandCapsuleModifier(isEnabled: useFloatingGlassBars))
            }
        }
        .fixedSize(horizontal: true, vertical: false)
    }
}

private struct BrandIslandCapsuleModifier: ViewModifier {
    let isEnabled: Bool
    
    func body(content: Content) -> some View {
        if isEnabled {
            content.nxGlassBar(in: Capsule())
        } else {
            content
        }
    }
}
