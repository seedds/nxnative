import SwiftUI
import AppKit

public struct SystemCreditsCard: View {
    public init() {}
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label("Acknowledgements & Credits", systemImage: "heart.fill")
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(Color(red: 0.95, green: 0.4, blue: 0.6))
                    
                    Spacer()
                    
                    Link(destination: URL(string: "https://github.com/seedds/nxnative")!) {
                        HStack(spacing: 4) {
                            Text("seedds/nxnative")
                            Image(systemName: "arrow.up.right")
                        }
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundColor(NXTheme.secondary)
                    }
                }
                
                Text("NxNative Launcher is a native macOS graphical interface and game library built on top of the breakthrough translation engine created by seedds and foundational open-source engineering.")
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
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
}
