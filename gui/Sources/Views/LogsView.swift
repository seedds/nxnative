import SwiftUI
import AppKit

public struct LogsView: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @State private var searchText: String = ""
    @State private var autoScroll: Bool = true
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var filteredLogs: [LogEntry] {
        if searchText.trimmingCharacters(in: .whitespaces).isEmpty {
            return viewModel.logs
        }
        return viewModel.logs.filter { $0.message.localizedCaseInsensitiveContains(searchText) }
    }
    
    private var useFloatingGlass: Bool {
        glassEnv.isSupportedAndEnabled && !reduceTransparency
    }
    
    public var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 4) {
                    if filteredLogs.isEmpty {
                        VStack(spacing: 8) {
                            Image(systemName: "terminal")
                                .font(.system(size: 28))
                                .foregroundColor(NXTheme.secondary.opacity(0.4))
                            Text(viewModel.tr("logs.empty"))
                                .font(.system(size: 12))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        .frame(maxWidth: .infinity, minHeight: 260)
                    } else {
                        ForEach(filteredLogs) { entry in
                            HStack(alignment: .top, spacing: 8) {
                                Text(entry.formattedTimestamp)
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundColor(NXTheme.textMuted)
                                
                                Image(systemName: entry.level.sfSymbol)
                                    .font(.system(size: 10))
                                    .foregroundColor(textColorFor(entry.level))
                                    .padding(.top, 2)
                                
                                Text(entry.message)
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundColor(textColorFor(entry.level))
                                    .textSelection(.enabled)
                                
                                Spacer()
                            }
                            .padding(.horizontal, 18)
                            .padding(.vertical, 2)
                            .id(entry.id)
                        }
                    }
                }
                .padding(.vertical, 12)
            }
            .nxScrollEdgeEffect(edges: [.top, .bottom])
            .safeAreaInset(edge: .top, spacing: 0) {
                logsToolbar
            }
            .background(
                Group {
                    if !useFloatingGlass {
                        NXTheme.canvasObsidian.opacity(0.5)
                    }
                }
            )
            .onChange(of: viewModel.logs.count) { _ in
                if autoScroll, let last = filteredLogs.last {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
        }
    }
    
    private var logsToolbar: some View {
        NXGlassContainer(spacing: 10) {
            HStack(spacing: 12) {
                NXLiquidGlassSearchField(
                    viewModel.tr("logs.filter"),
                    text: $searchText
                )
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .frame(maxWidth: 260)
                .nxGlassCard(cornerRadius: 16, isNested: true)
                
                Spacer()
                
                Toggle(viewModel.tr("logs.auto_scroll"), isOn: $autoScroll)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(NXTheme.textSecondary)
                
                Button(action: copyLogsToClipboard) {
                    Label(viewModel.tr("logs.copy"), systemImage: "doc.on.doc")
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
                
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    viewModel.logs.removeAll()
                }) {
                    Label(viewModel.tr("logs.clear"), systemImage: "trash")
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 9)
            .modifier(LogsToolbarSurfaceModifier(useFloatingGlass: useFloatingGlass))
        }
        .padding(.horizontal, useFloatingGlass ? 16 : 0)
        .padding(.top, useFloatingGlass ? 4 : 0)
        .padding(.bottom, useFloatingGlass ? 6 : 0)
    }
    
    private func textColorFor(_ level: LogLevel) -> Color {
        switch level {
        case .info: return NXTheme.textPrimary
        case .step: return NXTheme.secondary
        case .success: return Color(red: 0.13, green: 0.77, blue: 0.37)
        case .warning: return Color(red: 1.0, green: 0.65, blue: 0.35)
        case .error: return NXTheme.tertiary
        }
    }
    
    private func copyLogsToClipboard() {
        let full = viewModel.logs.map { "[\($0.formattedTimestamp)] \($0.message)" }.joined(separator: "\n")
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(full, forType: .string)
    }
}

private struct LogsToolbarSurfaceModifier: ViewModifier {
    let useFloatingGlass: Bool
    
    func body(content: Content) -> some View {
        if useFloatingGlass {
            content.nxGlassBar(in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        } else {
            content
                .background(
                    ZStack {
                        NXTheme.canvasObsidian.opacity(0.6)
                        Rectangle().fill(.ultraThinMaterial)
                    }
                )
                .overlay(
                    Rectangle()
                        .frame(height: 1)
                        .foregroundColor(NXTheme.borderSubtle),
                    alignment: .bottom
                )
        }
    }
}
