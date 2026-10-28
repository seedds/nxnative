import SwiftUI

public struct SystemStorageAndDiagnosticsCard: View {
    @ObservedObject public var viewModel: AppViewModel
    @State private var copiedReport: Bool = false
    @State private var freedSpaceMessage: String? = nil

    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }

    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label(viewModel.tr("system.storage_diag_title"), systemImage: "cylinder.split.1x2.fill")
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)

                    Spacer()
                }

                Divider().opacity(0.2)

                // Row 1: Diagnostics Report
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(viewModel.tr("system.github_diag"))
                            .font(.system(size: 12, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        Text(viewModel.tr("system.github_diag_desc"))
                            .font(.system(size: 11))
                            .foregroundColor(NXTheme.textSecondary)
                    }

                    Spacer()

                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        let logs = viewModel.logs.map { "[\($0.level.rawValue.uppercased())] \($0.message)" }
                        _ = DiagnosticsService.shared.generateAndCopyReport(lastLogLines: logs)
                        copiedReport = true
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
                            copiedReport = false
                        }
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: copiedReport ? "checkmark.circle.fill" : "doc.on.doc.fill")
                                .foregroundColor(copiedReport ? Color(red: 0.13, green: 0.77, blue: 0.37) : .white)
                            Text(copiedReport ? "Copied!" : "Copy Report")
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                }

                Divider().opacity(0.2)

                // Row 2: Free Space & Intermediate Caches Purge
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(viewModel.tr("system.build_caches"))
                            .font(.system(size: 12, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        Text(freedSpaceMessage ?? "Purges intermediate extraction, shaders, and temporary caches while preserving saves and finished apps.")
                            .font(.system(size: 11))
                            .foregroundColor(freedSpaceMessage != nil ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.textSecondary)
                    }

                    Spacer()

                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        let outDir = viewModel.customOutputFolder ?? (viewModel.selectedGameFolder ?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Desktop"))
                        let freed = BuildCleanupService.shared.cleanAllReclaimable(in: outDir)
                        if freed > 0 {
                            let str = ByteCountFormatter.string(fromByteCount: freed, countStyle: .file)
                            freedSpaceMessage = "Successfully freed ~\(str) of disk space!"
                        } else {
                            freedSpaceMessage = "Build folder is clean. No intermediate files to purge."
                        }
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "trash.fill")
                            Text(viewModel.tr("system.purge_caches"))
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                }
            }
        }
    }
}
