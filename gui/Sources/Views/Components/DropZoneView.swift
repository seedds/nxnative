import SwiftUI
import UniformTypeIdentifiers

public struct DropZoneView: View {
    @ObservedObject public var viewModel: AppViewModel
    @State private var isTargeted: Bool = false
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        VStack(spacing: 12) {
            if viewModel.isExtractingZip {
                VStack(spacing: 10) {
                    ProgressView()
                        .controlSize(.regular)
                    Text(viewModel.zipExtractionMessage)
                        .font(.system(size: 13, weight: .semibold, design: .rounded))
                        .foregroundColor(NXTheme.secondary)
                }
                .frame(maxWidth: .infinity, minHeight: 110)
            } else {
                VStack(spacing: 10) {
                    Image(systemName: isTargeted ? "arrow.down.doc.fill" : "square.and.arrow.down.on.square.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 34))
                        .foregroundColor(isTargeted ? NXTheme.secondary : NXTheme.textSecondary)
                        .scaleEffect(isTargeted ? 1.15 : 1.0)
                        .animation(.spring(response: 0.3, dampingFraction: 0.6), value: isTargeted)
                    
                    VStack(spacing: 4) {
                        Text(isTargeted ? viewModel.tr("build.dropzone_title") : viewModel.tr("build.dropzone_subtitle"))
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        
                        Text(viewModel.tr("build.dropzone_formats"))
                            .font(.system(size: 11))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    Button(viewModel.tr("common.choose_folder")) {
                        viewModel.selectGameFolder()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.small)
                    .padding(.top, 4)
                    
                    if let folder = viewModel.selectedGameFolder {
                        HStack(spacing: 8) {
                            Image(systemName: "folder.fill")
                                .foregroundColor(NXTheme.secondary)
                            Text(viewModel.tr("build.active_folder", folder.path))
                                .font(.system(size: 11, weight: .medium, design: .monospaced))
                                .foregroundColor(NXTheme.textPrimary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                            Spacer()
                            
                            Button(viewModel.tr("common.clear")) {
                                viewModel.selectedGameFolder = nil
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            .controlSize(.mini)
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                        .background(NXTheme.primary.opacity(0.12))
                        .cornerRadius(8)
                        .overlay(
                            RoundedRectangle(cornerRadius: 8, style: .continuous)
                                .strokeBorder(NXTheme.primary.opacity(0.35), lineWidth: 1)
                        )
                    }
                }
                .frame(maxWidth: .infinity, minHeight: 120)
            }
            
            if let err = viewModel.dropErrorMessage {
                HStack(spacing: 6) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(NXTheme.tertiary)
                    Text(err)
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.tertiary)
                }
                .padding(6)
                .background(NXTheme.tertiary.opacity(0.12))
                .cornerRadius(6)
            }
        }
        .padding(16)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(isTargeted ? NXTheme.primary.opacity(0.16) : NXTheme.glassSurfaceElevated.opacity(0.7))
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(
                    isTargeted ? NXTheme.secondary : NXTheme.borderSubtle,
                    style: StrokeStyle(lineWidth: isTargeted ? 1.8 : 1, dash: isTargeted ? [6] : [])
                )
        )
        .onDrop(of: [.fileURL], isTargeted: $isTargeted) { providers in
            var urls: [URL] = []
            let group = DispatchGroup()
            
            for provider in providers {
                group.enter()
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    defer { group.leave() }
                    if let fileURL = url {
                        urls.append(fileURL)
                    }
                }
            }
            
            group.notify(queue: .main) {
                if !urls.isEmpty {
                    viewModel.handleDroppedURLs(urls)
                }
            }
            return true
        }
    }
}
