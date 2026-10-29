import SwiftUI

public struct SystemStorageAndDiagnosticsCard: View {
    @ObservedObject public var viewModel: AppViewModel
    @State private var copiedReport: Bool = false
    @State private var freedSpaceMessage: String? = nil
    @State private var imageCacheBytes: Int64 = 0
    @State private var coverFreedMessage: String? = nil
    @State private var isClearingCovers: Bool = false

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

                // Row 2: Image & Cover Artwork Cache (MB / GB)
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 8) {
                            Text(viewModel.tr("system.image_cache_title"))
                                .font(.system(size: 12, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textPrimary)

                            Text(ByteCountFormatter.string(fromByteCount: imageCacheBytes, countStyle: .file))
                                .font(.system(size: 10, weight: .bold, design: .monospaced))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(Color.blue.opacity(0.25))
                                .foregroundColor(Color(red: 0.4, green: 0.7, blue: 1.0))
                                .clipShape(RoundedRectangle(cornerRadius: 5))
                        }

                        Text(coverFreedMessage ?? viewModel.tr("system.image_cache_desc"))
                            .font(.system(size: 11))
                            .foregroundColor(coverFreedMessage != nil ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.textSecondary)
                    }

                    Spacer()

                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        isClearingCovers = true
                        DispatchQueue.global(qos: .userInitiated).async {
                            let freed = BuildCleanupService.shared.cleanImageCache()
                            let freedStr = ByteCountFormatter.string(fromByteCount: freed, countStyle: .file)
                            DispatchQueue.main.async {
                                self.coverFreedMessage = "Successfully cleared ~\(freedStr) of image cache!"
                                self.imageCacheBytes = BuildCleanupService.shared.calculateImageCacheBytes()
                                self.isClearingCovers = false
                            }
                        }
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "photo.stack")
                            Text(viewModel.tr("system.purge_image_cache"))
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .disabled(isClearingCovers || imageCacheBytes == 0)
                }

                Divider().opacity(0.2)

                // Row 3: Free Space & Intermediate Caches Purge
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
        .onAppear {
            refreshImageCacheSize()
        }
    }

    private func refreshImageCacheSize() {
        DispatchQueue.global(qos: .utility).async {
            let bytes = BuildCleanupService.shared.calculateImageCacheBytes()
            DispatchQueue.main.async {
                self.imageCacheBytes = bytes
            }
        }
    }
}
