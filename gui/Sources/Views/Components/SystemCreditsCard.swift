import SwiftUI
import AppKit

public struct SystemCreditsCard: View {
    public init() {}
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label(L10n.t("system.credits_title"), systemImage: "heart.fill")
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(Color(red: 0.95, green: 0.4, blue: 0.6))
                    
                    Spacer()
                    
                    HStack(spacing: 12) {
                        Link(destination: URL(string: "https://github.com/rennan101/nxnative_Launcher")!) {
                            HStack(spacing: 4) {
                                Text("rennan101/nxnative_Launcher")
                                Image(systemName: "arrow.up.right")
                            }
                            .font(.system(size: 10, weight: .semibold))
                            .foregroundColor(NXTheme.primary)
                        }
                        
                        Link(destination: URL(string: "https://github.com/seedds/nxnative")!) {
                            HStack(spacing: 4) {
                                Text("seedds/nxnative")
                                Image(systemName: "arrow.up.right")
                            }
                            .font(.system(size: 10, weight: .semibold))
                            .foregroundColor(NXTheme.secondary)
                        }
                    }
                }
                
                // Highlight Card for Launcher Developer (Rennan)
                Button(action: {
                    if let url = URL(string: "https://github.com/rennan101/nxnative_Launcher") {
                        NSWorkspace.shared.open(url)
                    }
                }) {
                    HStack(spacing: 12) {
                        ZStack {
                            Circle()
                                .fill(NXTheme.primaryGradient)
                                .frame(width: 36, height: 36)
                            Image(systemName: "sparkles")
                                .font(.system(size: 15, weight: .bold))
                                .foregroundColor(.white)
                        }
                        
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(spacing: 6) {
                                Text("Rennan (@rennan101)")
                                    .font(.system(size: 12, weight: .bold, design: .rounded))
                                    .foregroundColor(.white)
                                Text(L10n.t("about.role_launcher_dev"))
                                    .font(.system(size: 8.5, weight: .heavy, design: .rounded))
                                    .foregroundColor(.white)
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(NXTheme.primary.opacity(0.6))
                                    .clipShape(Capsule())
                            }
                            Text(L10n.t("about.desc_launcher"))
                                .font(.system(size: 10))
                                .foregroundColor(NXTheme.textSecondary)
                                .lineLimit(1)
                        }
                        
                        Spacer()
                        
                        Image(systemName: "arrow.up.right.circle.fill")
                            .font(.system(size: 15))
                            .foregroundColor(NXTheme.secondary)
                    }
                    .padding(10)
                    .background(
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .fill(NXTheme.primary.opacity(0.14))
                            .overlay(
                                RoundedRectangle(cornerRadius: 8, style: .continuous)
                                    .strokeBorder(NXTheme.primary.opacity(0.4), lineWidth: 1)
                            )
                    )
                }
                .buttonStyle(NXPressableButtonStyle())
                .nxNoFocusRing()
                
                Text(L10n.t("about.description"))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
                
                Divider().opacity(0.18)
                
                // Project Credits Grid
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                    creditRow(title: "seedds", role: "nxnative Engine, NVN/Metal host & ARM64 rewriter", url: "https://github.com/seedds/nxnative")
                    creditRow(title: "d2r-mobile", role: "Foundational native execution architecture", url: "https://github.com/jhaizhou-ops/d2r-mobile")
                    creditRow(title: "Ryujinx Team", role: "Maxwell shader translator & SPIR-V generator", url: "https://github.com/Ryujinx/Ryujinx")
                    creditRow(title: "Atmosphère NX", role: "Mesosphère kernel synchronization semantics", url: "https://github.com/Atmosphere-NX/Atmosphere")
                    creditRow(title: "LibHac", role: "NSP reading and PFS/HFS layered filesystem", url: "https://github.com/Thealexbarney/LibHac")
                    creditRow(title: "Khronos Group", role: "SPIRV-Cross Metal Shading Language compiler", url: "https://github.com/KhronosGroup/SPIRV-Cross")
                }
            }
        }
    }
    
    private func creditRow(title: String, role: String, url: String) -> some View {
        Button(action: {
            if let targetURL = URL(string: url) {
                NSWorkspace.shared.open(targetURL)
            }
        }) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 4) {
                    Text(title)
                        .font(.system(size: 11, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    Image(systemName: "arrow.up.right")
                        .font(.system(size: 8))
                        .foregroundColor(NXTheme.textMuted)
                }
                Text(role)
                    .font(.system(size: 9))
                    .foregroundColor(NXTheme.textSecondary)
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(8)
            .background(NXTheme.glassSurfaceElevated.opacity(0.5))
            .cornerRadius(6)
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
    }
}
