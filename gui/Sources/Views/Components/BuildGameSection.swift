import SwiftUI

public struct BuildGameSection: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label(viewModel.tr("build.step_1_title"), systemImage: "gamecontroller.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 14, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    Spacer()
                }
                
                // Drag & Drop Area
                DropZoneView(viewModel: viewModel)
                
                if viewModel.selectedGameFolder != nil {
                    if viewModel.detectedFiles.isEmpty {
                        noFilesWarning
                    } else {
                        detectedGamesList
                    }
                }
                
                // Storage Space Status Alert
                storageStatusView
                
                // Pre-flight Cryptographic Keys & File Check
                keyCheckStatusView
                
                // Continue to Step 2 Action
                if viewModel.selectedGame != nil {
                    HStack {
                        Spacer()
                        Button(action: {
                            withAnimation(.easeInOut(duration: 0.25)) {
                                viewModel.goToWizardStep(2)
                            }
                        }) {
                            HStack(spacing: 8) {
                                Text(viewModel.tr("build.continue_config"))
                                Image(systemName: "arrow.right.circle.fill")
                            }
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .padding(.horizontal, 18)
                            .padding(.vertical, 8)
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    }
                    .padding(.top, 4)
                }
            }
        }
    }
    
    private var noFilesWarning: some View {
        HStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(NXTheme.tertiary)
            Text(viewModel.tr("build.no_files_found"))
                .font(.system(size: 12))
                .foregroundColor(NXTheme.textSecondary)
        }
        .padding(8)
        .background(NXTheme.tertiary.opacity(0.12))
        .cornerRadius(8)
    }
    
    private var detectedGamesList: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(viewModel.tr("build.select_game_detected", viewModel.detectedGames.count))
                    .font(.system(size: 12, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Spacer()
            }
            
            VStack(spacing: 6) {
                ForEach(viewModel.detectedGames) { game in
                    SelectableGameCard(
                        game: game,
                        isSelected: viewModel.selectedGame?.id == game.id,
                        onSelect: {
                            viewModel.selectedGame = game
                        }
                    )
                }
            }
        }
        .padding(.top, 4)
    }
    
    @ViewBuilder
    private var storageStatusView: some View {
        if let diskWarning = viewModel.diskSpaceWarningMessage {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "externaldrive.badge.xmark")
                    .font(.system(size: 20))
                    .foregroundColor(NXTheme.tertiary)
                
                VStack(alignment: .leading, spacing: 2) {
                    Text(viewModel.tr("build.low_disk_title"))
                        .font(.system(size: 12, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.tertiary)
                    Text(diskWarning)
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.textSecondary)
                }
            }
            .padding(10)
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(NXTheme.tertiary.opacity(0.12))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(NXTheme.tertiary.opacity(0.35), lineWidth: 1)
            )
        } else if let avail = viewModel.availableDiskBytes, let sel = viewModel.selectedGame {
            let availStr = ByteCountFormatter.string(fromByteCount: avail, countStyle: .file)
            HStack(spacing: 8) {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                    .font(.system(size: 14))
                Text(viewModel.tr("build.storage_ready", availStr, sel.name))
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(NXTheme.textSecondary)
            }
            .padding(.horizontal, 4)
        }
    }
    
    @ViewBuilder
    private var keyCheckStatusView: some View {
        if let sel = viewModel.selectedGame, !viewModel.prodKeysPath.isEmpty {
            let keysUrl = URL(fileURLWithPath: viewModel.prodKeysPath)
            let files = sel.files.map(\.url)
            let result = KeyCheckService.shared.verify(keysURL: keysUrl, gameFiles: files)
            
            HStack(spacing: 8) {
                switch result {
                case .fits(let fw, _):
                    Image(systemName: "key.fill")
                        .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                    Text(viewModel.tr("build.prodkeys_verified", fw))
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                case .outdatedMasterKey(let gen, let missing, let fw):
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(Color(red: 0.98, green: 0.65, blue: 0.12))
                    Text(viewModel.tr("build.prodkeys_missing_key", missing, String(gen), fw))
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(Color(red: 0.98, green: 0.65, blue: 0.12))
                case .wrongHeaderKey:
                    Image(systemName: "xmark.octagon.fill")
                        .foregroundColor(Color(red: 0.91, green: 0.14, blue: 0.14))
                    Text(viewModel.tr("build.prodkeys_invalid_header"))
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(Color(red: 0.91, green: 0.14, blue: 0.14))
                case .noHeaderKey:
                    Image(systemName: "xmark.octagon.fill")
                        .foregroundColor(Color(red: 0.91, green: 0.14, blue: 0.14))
                    Text(viewModel.tr("build.prodkeys_missing_header"))
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(Color(red: 0.91, green: 0.14, blue: 0.14))
                case .unreadableKeys:
                    Image(systemName: "questionmark.circle.fill")
                        .foregroundColor(NXTheme.textMuted)
                    Text(viewModel.tr("build.prodkeys_unparseable"))
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.textMuted)
                case .cannotCheck:
                    EmptyView()
                }
            }
            .padding(8)
            .background(NXTheme.glassSurfaceElevated.opacity(0.6))
            .cornerRadius(8)
        }
    }
}
