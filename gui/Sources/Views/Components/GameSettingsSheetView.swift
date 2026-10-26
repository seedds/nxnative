import SwiftUI
import AppKit

public struct GameSettingsSheetView: View {
    @ObservedObject public var viewModel: AppViewModel
    public let game: PortedGame
    
    @State private var config: GameRuntimeConfig
    @State private var showSavedAlert: Bool = false
    @Environment(\.dismiss) private var dismiss
    
    public init(viewModel: AppViewModel, game: PortedGame) {
        self.viewModel = viewModel
        self.game = game
        _config = State(initialValue: viewModel.loadRuntimeConfig(for: game))
    }
    
    public var body: some View {
        ZStack {
            NXTheme.canvasObsidian.ignoresSafeArea()
            
            VStack(spacing: 0) {
                // MARK: - Header
                HStack(spacing: 14) {
                    let icon = NSWorkspace.shared.icon(forFile: game.url.path)
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
                                .foregroundColor(NXTheme.secondary)
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
                    
                    Button(action: { dismiss() }) {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 18))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    .buttonStyle(.plain)
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
                        // Section 1: Framerate (60 FPS)
                        settingCard(
                            title: "Framerate Target",
                            icon: "speedometer",
                            iconColor: Color(red: 0.13, green: 0.77, blue: 0.37)
                        ) {
                            Toggle(isOn: $config.enable60FPS) {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(config.enable60FPS ? "60 FPS (Unlocked)" : "30 FPS (Switch Default)")
                                        .font(.system(size: 13, weight: .semibold, design: .rounded))
                                        .foregroundColor(NXTheme.textPrimary)
                                    Text("Unlocks the game's internal frame limiter to 60 FPS with smooth VSync.")
                                        .font(.system(size: 11))
                                        .foregroundColor(NXTheme.textSecondary)
                                }
                            }
                            .toggleStyle(.switch)
                        }
                        
                        // Section 2: Metal HUD
                        settingCard(
                            title: "Performance Overlay (Metal HUD)",
                            icon: "gauge.with.needle.fill",
                            iconColor: NXTheme.secondary
                        ) {
                            Toggle(isOn: $config.enableMetalHUD) {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(config.enableMetalHUD ? "Metal HUD Enabled" : "Metal HUD Disabled")
                                        .font(.system(size: 13, weight: .semibold, design: .rounded))
                                        .foregroundColor(NXTheme.textPrimary)
                                    Text("Displays Apple's official in-game performance overlay with real-time FPS and GPU metrics.")
                                        .font(.system(size: 11))
                                        .foregroundColor(NXTheme.textSecondary)
                                }
                            }
                            .toggleStyle(.switch)
                        }
                        
                        // Section 3: Console Mode
                        settingCard(
                            title: "Console Mode (Operation Profile)",
                            icon: "tv.and.mediabox",
                            iconColor: NXTheme.primary
                        ) {
                            VStack(alignment: .leading, spacing: 8) {
                                Picker("", selection: $config.isDockedMode) {
                                    Text("Docked (TV Mode · 1080p)").tag(true)
                                    Text("Handheld (Portable · 720p)").tag(false)
                                }
                                .pickerStyle(.segmented)
                                
                                Text(config.isDockedMode
                                     ? "Docked Mode: Uses maximum graphics profile and render resolution (1080p/900p)."
                                     : "Handheld Mode: Renders at native 720p with reduced GPU workload.")
                                    .font(.system(size: 11))
                                    .foregroundColor(NXTheme.textSecondary)
                            }
                        }
                        
                        // Section 4: 3D Render Scale
                        settingCard(
                            title: "3D Render Scale",
                            icon: "viewfinder.circle.fill",
                            iconColor: Color(red: 1.0, green: 0.65, blue: 0.35)
                        ) {
                            VStack(alignment: .leading, spacing: 10) {
                                HStack {
                                    Text("Render Resolution Scale:")
                                        .font(.system(size: 12, weight: .medium))
                                        .foregroundColor(NXTheme.textSecondary)
                                    Spacer()
                                    Text("\(Int(config.renderScale * 100))%")
                                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                                        .foregroundColor(NXTheme.secondary)
                                }
                                
                                Slider(value: $config.renderScale, in: 0.5...1.0, step: 0.05)
                                    .tint(NXTheme.primary)
                                
                                HStack(spacing: 8) {
                                    Button("50% (Performance)") { config.renderScale = 0.5 }
                                        .buttonStyle(NXSecondaryButtonStyle())
                                        .controlSize(.mini)
                                    Button("75% (Balanced)") { config.renderScale = 0.75 }
                                        .buttonStyle(NXSecondaryButtonStyle())
                                        .controlSize(.mini)
                                    Button("100% (Native)") { config.renderScale = 1.0 }
                                        .buttonStyle(NXSecondaryButtonStyle())
                                        .controlSize(.mini)
                                }
                            }
                        }
                        
                        // Section 5: MetalFX Upscaling
                        settingCard(
                            title: "Intelligent Upscaling (MetalFX)",
                            icon: "sparkles.tv.fill",
                            iconColor: NXTheme.secondary
                        ) {
                            Toggle(isOn: $config.enableMetalFXUpscale) {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(config.enableMetalFXUpscale ? "MetalFX Spatial Upscaler Enabled" : "Standard Bilinear Upscaling")
                                        .font(.system(size: 13, weight: .semibold, design: .rounded))
                                        .foregroundColor(NXTheme.textPrimary)
                                    Text("Uses Apple Silicon spatial neural upscaling for crisp high-resolution presentation.")
                                        .font(.system(size: 11))
                                        .foregroundColor(NXTheme.textSecondary)
                                }
                            }
                            .toggleStyle(.switch)
                        }
                    }
                    .padding(20)
                }
                
                // MARK: - Footer Actions
                HStack(spacing: 12) {
                    Button("Restore Defaults") {
                        config = GameRuntimeConfig()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.regular)
                    
                    Spacer()
                    
                    Button("Cancel") {
                        dismiss()
                    }
                    .buttonStyle(.plain)
                    .foregroundColor(NXTheme.textSecondary)
                    .font(.system(size: 12))
                    .padding(.horizontal, 8)
                    
                    Button("Save Settings") {
                        viewModel.saveRuntimeConfig(config, for: game)
                        dismiss()
                    }
                    .buttonStyle(NXPrimaryButtonStyle())
                    .controlSize(.regular)
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 14)
                .background(
                    ZStack {
                        NXTheme.glassSurfaceElevated.opacity(0.8)
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
        .frame(minWidth: 480, idealWidth: 540, maxWidth: 620, minHeight: 540, idealHeight: 620, maxHeight: 720)
    }
    
    private func settingCard<Content: View>(
        title: String,
        icon: String,
        iconColor: Color,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Image(systemName: icon)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(iconColor)
                Text(title)
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Spacer()
            }
            
            content()
        }
        .padding(14)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(NXTheme.glassSurfaceElevated.opacity(0.7))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }
}
