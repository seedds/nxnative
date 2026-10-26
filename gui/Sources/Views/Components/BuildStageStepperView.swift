import SwiftUI

public struct BuildStageStepperView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Porting Stages (\(viewModel.stageIndex)/\(max(viewModel.totalStages, viewModel.buildStages.count))):")
                .font(.system(size: 11, weight: .bold))
                .foregroundColor(NXTheme.textSecondary)
            
            VStack(spacing: 6) {
                ForEach(viewModel.buildStages) { stage in
                    stageRow(stage)
                }
            }
        }
        .padding(10)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(NXTheme.glassSurfaceElevated.opacity(0.8))
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }
    
    private func stageRow(_ stage: BuildStageItem) -> some View {
        HStack(spacing: 8) {
            if stage.isRunning {
                ProgressView()
                    .controlSize(.mini)
                    .frame(width: 16)
            } else if stage.isCompleted {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                    .font(.system(size: 13))
                    .frame(width: 16)
            } else if stage.isFailed {
                Image(systemName: "xmark.circle.fill")
                    .foregroundColor(.red)
                    .font(.system(size: 13))
                    .frame(width: 16)
            } else {
                Circle()
                    .stroke(NXTheme.textSecondary.opacity(0.4), lineWidth: 1.5)
                    .frame(width: 10, height: 10)
                    .padding(.horizontal, 3)
            }
            
            Text("Stage \(stage.id): \(stage.name)")
                .font(.system(size: 12, weight: stage.isRunning ? .bold : .medium, design: .rounded))
                .foregroundColor(stage.isRunning ? NXTheme.textPrimary : (stage.isCompleted ? NXTheme.textSecondary : NXTheme.textMuted))
            
            if !stage.step.isEmpty && stage.isRunning {
                Text("(\(stage.step))")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(NXTheme.secondary)
                    .lineLimit(1)
            }
            
            Spacer()
            
            if let elapsed = stage.elapsedSeconds, elapsed > 0 {
                Text(String(format: "%.1fs", elapsed))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(NXTheme.textMuted)
            }
        }
        .padding(.vertical, 2)
    }
}
