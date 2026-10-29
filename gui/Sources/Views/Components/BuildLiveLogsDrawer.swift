import SwiftUI

public struct BuildLiveLogsDrawer: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label(viewModel.tr("build.logs"), systemImage: "terminal.fill")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundColor(NXTheme.textSecondary)
                Spacer()
                Text(viewModel.tr("build.lines_count", viewModel.logs.count))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(NXTheme.textMuted)
            }
            
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 3) {
                        let recentLogs = viewModel.logs.suffix(60)
                        ForEach(Array(recentLogs), id: \.id) { log in
                            HStack(alignment: .top, spacing: 6) {
                                Text(log.message)
                                    .font(.system(size: 10, design: .monospaced))
                                    .foregroundColor(logColor(log.level))
                                    .lineLimit(2)
                                    .textSelection(.enabled)
                                Spacer()
                            }
                            .id(log.id)
                        }
                    }
                    .padding(8)
                }
                .frame(height: 130)
                .background(NXTheme.canvasObsidian.opacity(0.75))
                .cornerRadius(8)
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
                )
                .onChange(of: viewModel.logs.count) { _ in
                    let recentCount = viewModel.logs.suffix(60).count
                    if recentCount > 0 {
                        proxy.scrollTo(recentCount - 1, anchor: .bottom)
                    }
                }
            }
        }
    }
    
    private func logColor(_ level: LogLevel) -> Color {
        switch level {
        case .info: return NXTheme.textPrimary.opacity(0.85)
        case .step: return NXTheme.secondary
        case .success: return Color(red: 0.13, green: 0.77, blue: 0.37)
        case .warning: return NXTheme.tertiary
        case .error: return .red
        }
    }
}
