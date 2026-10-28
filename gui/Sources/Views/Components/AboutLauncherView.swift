import SwiftUI
import AppKit

public struct AboutLauncherView: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.dismiss) private var dismiss
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var appVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.5.1"
    }
    
    private var currentCommit: String {
        if let commit = viewModel.updateInfo?.localCommit, !commit.isEmpty {
            return commit
        }
        return UpdateCheckerService.shared.getLocalCommit(nxnativePath: viewModel.nxnativePath)
    }
    
    private var appIconImage: NSImage? {
        if let url = Bundle.main.url(forResource: "AppIcon-Light", withExtension: "png") ??
                     Bundle.main.url(forResource: "AppIcon", withExtension: "png"),
           let img = NSImage(contentsOf: url) {
            return img
        }
        return NSApp?.applicationIconImage
    }
    
    public var body: some View {
        ZStack {
            NXTheme.canvasObsidian.ignoresSafeArea()
            
            VStack(spacing: 14) {
                // MARK: - Clean App Header
                HStack(spacing: 14) {
                    if let icon = appIconImage {
                        Image(nsImage: icon)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .frame(width: 60, height: 60)
                            .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                            .overlay(
                                RoundedRectangle(cornerRadius: 14, style: .continuous)
                                    .strokeBorder(Color.white.opacity(0.22), lineWidth: 1)
                            )
                            .shadow(color: NXTheme.primary.opacity(0.35), radius: 8, x: 0, y: 3)
                    }
                    
                    VStack(alignment: .leading, spacing: 5) {
                        HStack(spacing: 8) {
                            Text("NxNative Launcher")
                                .font(.system(size: 18, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                            
                            HStack(spacing: 4) {
                                Image(systemName: "tag.fill")
                                    .font(.system(size: 8.5, weight: .bold))
                                Text("v\(appVersion) · \(currentCommit)")
                                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                            }
                            .foregroundColor(.white)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 3)
                            .background(NXTheme.primary.opacity(0.45))
                            .clipShape(Capsule())
                        }
                        
                        Text("Switch Native Port Builder & Library for Apple Silicon")
                            .font(.system(size: 11.5, weight: .medium))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    Spacer()
                }
                
                Divider().opacity(0.18)
                
                // MARK: - Launcher Developer (Rennan)
                HStack(spacing: 12) {
                    ZStack {
                        Circle()
                            .fill(NXTheme.primaryGradient)
                            .frame(width: 38, height: 38)
                        Image(systemName: "person.fill.checkmark")
                            .font(.system(size: 15, weight: .bold))
                            .foregroundColor(.white)
                    }
                    
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 6) {
                            Text("Rennan (@rennan101)")
                                .font(.system(size: 13.5, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                            
                            Text("Launcher Developer")
                                .font(.system(size: 9, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                                .padding(.horizontal, 7)
                                .padding(.vertical, 2)
                                .background(NXTheme.primary)
                                .clipShape(Capsule())
                        }
                        
                        Text("macOS Native GUI, Game Library, Auto-Updater & Controller UX")
                            .font(.system(size: 10.5))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    Spacer()
                    
                    Button(action: {
                        if let url = URL(string: "https://github.com/rennan101/nxnative_Launcher") {
                            NSWorkspace.shared.open(url)
                        }
                    }) {
                        HStack(spacing: 4) {
                            Image(systemName: "arrow.up.right")
                                .font(.system(size: 10, weight: .bold))
                            Text("GitHub")
                                .font(.system(size: 11, weight: .bold))
                        }
                        .foregroundColor(.white)
                    }
                    .buttonStyle(NXPrimaryButtonStyle())
                    .controlSize(.small)
                }
                .padding(11)
                .background(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(NXTheme.primary.opacity(0.15))
                        .overlay(
                            RoundedRectangle(cornerRadius: 10, style: .continuous)
                                .strokeBorder(NXTheme.primary.opacity(0.45), lineWidth: 1)
                        )
                )
                
                // MARK: - Open Source Credits
                VStack(alignment: .leading, spacing: 6) {
                    Text("CORE ENGINE & ACKNOWLEDGEMENTS")
                        .font(.system(size: 9.5, weight: .bold, design: .rounded))
                        .foregroundColor(.white.opacity(0.75))
                        .tracking(0.6)
                    
                    LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 6) {
                        creditPill(title: "seedds", role: "nxnative Core Engine", url: "https://github.com/seedds/nxnative")
                        creditPill(title: "d2r-mobile", role: "Execution Architecture", url: "https://github.com/jhaizhou-ops/d2r-mobile")
                        creditPill(title: "Ryujinx Team", role: "Shader Translator", url: "https://github.com/Ryujinx/Ryujinx")
                        creditPill(title: "Atmosphère NX", role: "Kernel Semantics", url: "https://github.com/Atmosphere-NX/Atmosphere")
                        creditPill(title: "LibHac", role: "NSP Filesystem", url: "https://github.com/Thealexbarney/LibHac")
                        creditPill(title: "Khronos & GameTDB", role: "SPIRV-Cross & Box Art", url: "https://github.com/KhronosGroup/SPIRV-Cross")
                    }
                }
                
                Divider().opacity(0.18)
                
                // MARK: - Footer
                HStack {
                    Button(action: {
                        dismiss()
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                            viewModel.openUpdateCheckerAndAutoInstall()
                        }
                    }) {
                        HStack(spacing: 5) {
                            Image(systemName: "arrow.triangle.2.circlepath")
                            Text("Check for Updates...")
                        }
                        .foregroundColor(.white)
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    
                    Spacer()
                    
                    Button("Close") {
                        dismiss()
                    }
                    .buttonStyle(NXPrimaryButtonStyle())
                    .keyboardShortcut(.defaultAction)
                }
            }
            .padding(20)
        }
        .frame(width: 470)
        .fixedSize(horizontal: false, vertical: true)
    }
    
    private func creditPill(title: String, role: String, url: String) -> some View {
        Button(action: {
            if let target = URL(string: url) {
                NSWorkspace.shared.open(target)
            }
        }) {
            HStack(spacing: 6) {
                VStack(alignment: .leading, spacing: 1) {
                    Text(title)
                        .font(.system(size: 11, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                    Text(role)
                        .font(.system(size: 9.5))
                        .foregroundColor(.white.opacity(0.85))
                        .lineLimit(1)
                }
                Spacer()
                Image(systemName: "arrow.up.right")
                    .font(.system(size: 9, weight: .bold))
                    .foregroundColor(.white)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(NXTheme.glassSurfaceElevated.opacity(0.75))
            .cornerRadius(8)
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(Color.white.opacity(0.18), lineWidth: 0.7)
            )
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
}
