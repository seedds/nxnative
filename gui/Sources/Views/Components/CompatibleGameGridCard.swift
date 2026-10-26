import SwiftUI
import AppKit

public struct CompatibleGameGridCard: View {
    public let item: CompatibleGameItem
    
    public init(item: CompatibleGameItem) {
        self.item = item
    }
    
    public var body: some View {
        GlassCard(padding: 14) {
            VStack(alignment: .leading, spacing: 10) {
                // Top Engine & Graphics API Pills
                HStack {
                    Text(item.engine)
                        .font(.system(size: 9, weight: .bold, design: .rounded))
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(NXTheme.primary.opacity(0.3))
                        .foregroundColor(.white)
                        .clipShape(Capsule())
                    
                    Spacer()
                    
                    Text(item.graphicsApi)
                        .font(.system(size: 9, weight: .bold, design: .monospaced))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(NXTheme.secondary.opacity(0.25))
                        .foregroundColor(NXTheme.secondary)
                        .clipShape(Capsule())
                }
                
                // Title & TitleID
                VStack(alignment: .leading, spacing: 3) {
                    Text(item.title)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .lineLimit(2)
                        .frame(minHeight: 34, alignment: .topLeading)
                    
                    Text(item.titleId)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(NXTheme.textMuted)
                }
                
                Divider().opacity(0.2)
                
                // Footer: Tested Version & Notes Link
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Tested Build")
                            .font(.system(size: 8, weight: .bold))
                            .foregroundColor(NXTheme.textMuted)
                        Text(item.versionTested.isEmpty ? "Base" : item.versionTested)
                            .font(.system(size: 10, weight: .semibold, design: .monospaced))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    Spacer()
                    
                    if let url = item.githubUrl {
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            NSWorkspace.shared.open(url)
                        }) {
                            HStack(spacing: 4) {
                                Text("Notes")
                                Image(systemName: "arrow.up.right")
                            }
                            .font(.system(size: 10, weight: .semibold))
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.small)
                    }
                }
            }
        }
    }
}
