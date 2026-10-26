import SwiftUI

public struct DependencyRow: View {
    public let item: DependencyStatus
    @ObservedObject public var viewModel: AppViewModel
    
    public init(item: DependencyStatus, viewModel: AppViewModel) {
        self.item = item
        self.viewModel = viewModel
    }
    
    public var body: some View {
        HStack(spacing: 12) {
            Image(systemName: item.isAvailable ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundColor(item.isAvailable ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.tertiary)
                .font(.system(size: 16))
                .frame(width: 20)
            
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(item.name)
                        .font(.system(size: 12, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    
                    if item.isProtected {
                        Text("System")
                            .font(.system(size: 9, weight: .bold))
                            .padding(.horizontal, 5)
                            .padding(.vertical, 1)
                            .background(NXTheme.primary.opacity(0.2))
                            .foregroundColor(NXTheme.secondary)
                            .clipShape(Capsule())
                    }
                }
                
                Text(item.details)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(NXTheme.textSecondary)
                
                if let rec = item.recommendation {
                    Text(rec)
                        .font(.system(size: 11))
                        .foregroundColor(Color(red: 1.0, green: 0.65, blue: 0.35))
                }
            }
            
            Spacer()
            
            // Actions: Install / Repair / Uninstall
            if !item.isAvailable && item.installCommand != nil {
                Button(action: {
                    viewModel.installDependency(item)
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: "arrow.down.circle")
                        Text("Install")
                    }
                    .font(.system(size: 11, weight: .semibold))
                }
                .buttonStyle(NXPrimaryButtonStyle())
                .controlSize(.small)
            } else if item.isAvailable {
                HStack(spacing: 6) {
                    Button("Repair") {
                        viewModel.repairDependency(item)
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.small)
                    .help("Reinstall / repair tool")
                    
                    if !item.isProtected && item.uninstallCommand != nil {
                        Button(role: .destructive) {
                            viewModel.confirmUninstall(item)
                        } label: {
                            Image(systemName: "trash")
                                .font(.system(size: 11))
                                .foregroundColor(NXTheme.tertiary)
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.small)
                        .help("Uninstall tool")
                    }
                }
            }
        }
        .padding(10)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(NXTheme.glassSurfaceElevated.opacity(0.65))
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }
}
