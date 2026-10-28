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
                Text("nxnative Core Repository Not Installed")
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("The conversion scripts are required to build native ports. Download with 1 click.")
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            if viewModel.isCloningRepository {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.small)
                    Text("Downloading...")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(NXTheme.secondary)
                }
            } else {
                Button("Download & Setup (1-Click)") {
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
                Text("Decryption Keys (prod.keys) Missing")
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Keys are required to decrypt game archives. Configure them once in Settings.")
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button("Configure in Settings") {
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
                Text("Apple Command Line Tools Not Installed")
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Apple developer compilers are required to build native Mac ports. Install with 1 click.")
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button("Install Apple Developer Tools") {
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
                Text("Homebrew Package Manager Not Installed")
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Homebrew is required to install Python modules, .NET, and build compilers. Install with 1 click.")
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button("Install Homebrew & Tools") {
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
                Text("Missing Build Dependencies (\(viewModel.prerequisites.missingList.count) Left)")
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Install all missing dependencies sequentially one by one until completed.")
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button("Install All One by One") {
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
