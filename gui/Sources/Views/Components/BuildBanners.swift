import SwiftUI

public struct BuildBanners: View {
    @ObservedObject public var viewModel: AppViewModel
    @Binding public var isCreditsDismissed: Bool
    
    public init(viewModel: AppViewModel, isCreditsDismissed: Binding<Bool>) {
        self.viewModel = viewModel
        self._isCreditsDismissed = isCreditsDismissed
    }
    
    public var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            // Git Repository Update Available
            if let update = viewModel.updateInfo, update.hasUpdate {
                updateBanner(update: update)
            }
            
            if let updateSuccess = viewModel.updateSuccessBanner {
                updateSuccessBanner(updateSuccess)
            }
            
            if let updateError = viewModel.updateErrorBanner {
                updateErrorBanner(updateError)
            }
            
            // Setup Quick-Fix Banners
            BuildPrerequisiteBanners(viewModel: viewModel)
            
            // Success & Error Banners
            if let successPath = viewModel.buildSuccessPath {
                successBanner(path: successPath)
            }
            
            if let error = viewModel.buildError {
                errorBanner(message: error)
            }
        }
    }
    
    // MARK: - Banner Subviews
    
    private var creditsBanner: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "heart.circle.fill")
                .symbolRenderingMode(.hierarchical)
                .font(.system(size: 24))
                .foregroundColor(NXTheme.secondary)
            
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(viewModel.tr("build.community_disclaimer_title"))
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    
                    Link("GitHub Repository ↗", destination: URL(string: "https://github.com/seedds/nxnative")!)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(NXTheme.secondary)
                }
                
                Text(viewModel.tr("build.community_disclaimer_desc"))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
                    .lineSpacing(2)
            }
            
            Spacer()
            
            Button(action: {
                withAnimation {
                    isCreditsDismissed = true
                }
            }) {
                Image(systemName: "xmark")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(NXTheme.textSecondary)
            }
            .buttonStyle(.plain)
            .padding(4)
        }
        .padding(14)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(NXTheme.primary.opacity(0.12))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.primary.opacity(0.35), lineWidth: 1)
        )
    }
    
    private func updateBanner(update: UpdateInfo) -> some View {
        HStack(spacing: 12) {
            Image(systemName: "arrow.triangle.2.circlepath.circle.fill")
                .symbolRenderingMode(.hierarchical)
                .font(.system(size: 26))
                .foregroundColor(NXTheme.secondary)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(viewModel.tr("build.update_banner_title"))
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(viewModel.tr("build.update_banner_commits", update.commitMessages.count))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            if viewModel.isApplyingUpdate {
                ProgressView().controlSize(.small)
            } else {
                Button(viewModel.tr("build.update_now")) {
                    viewModel.applyUpdate()
                }
                .buttonStyle(NXPrimaryButtonStyle())
                .controlSize(.regular)
            }
        }
        .padding(12)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(NXTheme.primary.opacity(0.12))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.secondary.opacity(0.4), lineWidth: 1)
        )
    }
    
    private func updateSuccessBanner(_ message: String) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "checkmark.seal.fill")
                .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                .font(.system(size: 18))
            Text(message)
                .font(.system(size: 12, weight: .medium))
                .foregroundColor(NXTheme.textPrimary)
            Spacer()
            Button(viewModel.tr("common.dismiss")) {
                viewModel.updateSuccessBanner = nil
            }
            .buttonStyle(.plain)
            .font(.system(size: 11, weight: .bold))
            .foregroundColor(NXTheme.textSecondary)
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.12))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.35), lineWidth: 1)
        )
    }
    
    private func updateErrorBanner(_ message: String) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(.orange)
                .font(.system(size: 18))
            Text(message)
                .font(.system(size: 12, weight: .medium))
                .foregroundColor(NXTheme.textPrimary)
            Spacer()
            Button(viewModel.tr("common.dismiss")) {
                viewModel.updateErrorBanner = nil
            }
            .buttonStyle(.plain)
            .font(.system(size: 11, weight: .bold))
            .foregroundColor(NXTheme.textSecondary)
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(Color.orange.opacity(0.12))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color.orange.opacity(0.35), lineWidth: 1)
        )
    }
    
    private func successBanner(path: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 28))
                .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
            
            VStack(alignment: .leading, spacing: 3) {
                Text(viewModel.tr("build.port_succeeded"))
                    .font(.system(size: 14, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(path)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(NXTheme.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            
            Spacer()
            
            if path.hasSuffix(".ipa") {
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    let url = URL(fileURLWithPath: path)
                    DeviceDeploymentService.shared.shareViaAirDrop(fileURL: url)
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: "airdrop")
                        Text(viewModel.tr("common.airdrop"))
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
            }
            
            Button(viewModel.tr("common.view_library")) {
                viewModel.selectedTab = 1
            }
            .buttonStyle(NXSecondaryButtonStyle())
            
            Button(viewModel.tr("common.reveal_finder")) {
                viewModel.openOutputInFinder()
            }
            .buttonStyle(NXPrimaryButtonStyle())
        }
        .padding(14)
        .background(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.14))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.4), lineWidth: 1)
        )
    }
    
    private func errorBanner(message: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: "xmark.octagon.fill")
                .font(.system(size: 24))
                .foregroundColor(Color.red)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(viewModel.tr("build.port_failed"))
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(message)
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
                    .lineLimit(2)
            }
            
            Spacer()
            
            Button(viewModel.tr("common.view_logs")) {
                viewModel.selectedTab = 6
            }
            .buttonStyle(NXSecondaryButtonStyle())
        }
        .padding(12)
        .background(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(Color.red.opacity(0.12))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color.red.opacity(0.35), lineWidth: 1)
        )
    }
}
