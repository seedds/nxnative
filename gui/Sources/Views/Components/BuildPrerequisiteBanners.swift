import SwiftUI

public struct BuildPrerequisiteBanners: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        VStack(spacing: 12) {
            if !viewModel.isNxnativeValid {
                nxnativeMissingBanner
            }
            
            if !viewModel.isProdKeysValid {
                prodKeysMissingBanner
            }
            
            if !viewModel.prerequisites.xcode.isAvailable {
                cltMissingBanner
            } else if !viewModel.prerequisites.homebrew.isAvailable {
                homebrewMissingBanner
            } else if !viewModel.prerequisites.allSatisfied {
                dependenciesMissingBanner
            }
        }
    }
    
    private var nxnativeMissingBanner: some View {
        HStack(spacing: 12) {
            Image(systemName: "arrow.down.circle.fill")
                .font(.system(size: 26))
                .foregroundColor(NXTheme.tertiary)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(viewModel.tr("build.core_repo_missing"))
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(viewModel.tr("build.core_repo_missing_desc"))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            if viewModel.isCloningRepository {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.small)
                    Text(viewModel.tr("common.downloading"))
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(NXTheme.secondary)
                }
            } else {
                Button(viewModel.tr("build.download_setup_click")) {
                    viewModel.cloneNxnativeRepository()
                }
                .buttonStyle(NXPrimaryButtonStyle())
                .controlSize(.regular)
            }
        }
        .padding(12)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(NXTheme.tertiary.opacity(0.12))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.tertiary.opacity(0.4), lineWidth: 1)
        )
    }
    
    private var prodKeysMissingBanner: some View {
        HStack(spacing: 12) {
            Image(systemName: "key.fill")
                .font(.system(size: 24))
                .foregroundColor(NXTheme.tertiary)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(viewModel.tr("build.keys_missing_title"))
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(viewModel.tr("build.keys_missing_desc"))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button(viewModel.tr("build.configure_settings")) {
                viewModel.selectedTab = 5
            }
            .buttonStyle(NXAccentButtonStyle())
            .controlSize(.regular)
        }
        .padding(12)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(NXTheme.tertiary.opacity(0.12))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.tertiary.opacity(0.4), lineWidth: 1)
        )
    }
    
    private var cltMissingBanner: some View {
        HStack(spacing: 12) {
            Image(systemName: "wrench.and.screwdriver.fill")
                .font(.system(size: 24))
                .foregroundColor(NXTheme.tertiary)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(viewModel.tr("build.apple_tools_missing"))
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(viewModel.tr("build.apple_tools_missing_desc"))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button(viewModel.tr("build.install_apple_tools")) {
                viewModel.installAppleCommandLineTools()
            }
            .buttonStyle(NXAccentButtonStyle())
            .controlSize(.regular)
        }
        .padding(12)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(NXTheme.tertiary.opacity(0.12))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.tertiary.opacity(0.4), lineWidth: 1)
        )
    }
    
    private var homebrewMissingBanner: some View {
        HStack(spacing: 12) {
            Image(systemName: "mug.fill")
                .font(.system(size: 24))
                .foregroundColor(NXTheme.tertiary)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(viewModel.tr("build.homebrew_missing"))
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(viewModel.tr("build.homebrew_missing_desc"))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button(viewModel.tr("build.install_homebrew")) {
                viewModel.installAllMissingDependencies()
            }
            .buttonStyle(NXAccentButtonStyle())
            .controlSize(.regular)
        }
        .padding(12)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(NXTheme.tertiary.opacity(0.12))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.tertiary.opacity(0.4), lineWidth: 1)
        )
    }
    
    private var dependenciesMissingBanner: some View {
        HStack(spacing: 12) {
            Image(systemName: "wrench.and.screwdriver.fill")
                .font(.system(size: 24))
                .foregroundColor(NXTheme.tertiary)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(viewModel.tr("build.deps_missing_title", viewModel.prerequisites.missingList.count))
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(viewModel.tr("build.deps_missing_desc"))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button(viewModel.tr("build.install_all_seq")) {
                viewModel.installAllMissingDependencies()
            }
            .buttonStyle(NXAccentButtonStyle())
            .controlSize(.regular)
        }
        .padding(12)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(NXTheme.tertiary.opacity(0.12))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(NXTheme.tertiary.opacity(0.4), lineWidth: 1)
        )
    }
}
