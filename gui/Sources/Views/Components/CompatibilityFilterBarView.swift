import SwiftUI

public struct CompatibilityFilterBarView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    private let alphabet = (65...90).map { String(UnicodeScalar($0)) }
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        VStack(spacing: 8) {
            // Row 1: RAM Filter Pills
            HStack(spacing: 8) {
                HStack(spacing: 5) {
                    Image(systemName: "memorychip")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(NXTheme.secondary)
                    Text("RAM:")
                        .font(.system(size: 11, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                }
                
                ramFilterPill(label: "All", ramGB: nil, count: viewModel.compatibleGames.count)
                ramFilterPill(label: "4GB", ramGB: 4, count: viewModel.compatibleGames.filter { $0.minRamGB <= 4 }.count)
                ramFilterPill(label: "6GB", ramGB: 6, count: viewModel.compatibleGames.filter { $0.minRamGB <= 6 }.count)
                ramFilterPill(label: "8GB", ramGB: 8, count: viewModel.compatibleGames.filter { $0.minRamGB <= 8 }.count)
                ramFilterPill(label: "16GB", ramGB: 16, count: viewModel.compatibleGames.filter { $0.minRamGB <= 16 }.count)
                
                Spacer()
                
                if viewModel.compatibilityRamFilter != nil || viewModel.compatibilityNameFilter != "All" {
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        withAnimation {
                            viewModel.compatibilityRamFilter = nil
                            viewModel.compatibilityNameFilter = "All"
                        }
                    }) {
                        HStack(spacing: 4) {
                            Image(systemName: "xmark.circle.fill")
                                .font(.system(size: 10))
                            Text("Reset")
                                .font(.system(size: 10, weight: .semibold))
                        }
                        .foregroundColor(NXTheme.textSecondary)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                        .background(NXTheme.glassSurfaceElevated.opacity(0.5))
                        .clipShape(Capsule())
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 18)
            
            // Row 2: Centered A-Z Small Buttons
            GeometryReader { geo in
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 3) {
                        nameLetterButton("All", isSelected: viewModel.compatibilityNameFilter == "All") {
                            viewModel.compatibilityNameFilter = "All"
                        }
                        
                        ForEach(alphabet, id: \.self) { letter in
                            nameLetterButton(letter, isSelected: viewModel.compatibilityNameFilter == letter) {
                                viewModel.compatibilityNameFilter = letter
                            }
                        }
                    }
                    .padding(.horizontal, 18)
                    .frame(minWidth: geo.size.width, alignment: .center)
                }
            }
            .frame(height: 24)
        }
        .padding(.vertical, 8)
        .background(NXTheme.glassSurface.opacity(0.35))
    }
    
    private func ramFilterPill(label: String, ramGB: Int?, count: Int) -> some View {
        let isSelected = viewModel.compatibilityRamFilter == ramGB
        return Button(action: {
            SoundEffectService.shared.play(.click)
            viewModel.compatibilityFocusArea = .filters
            if viewModel.compatibilityRamFilter == ramGB && ramGB != nil {
                viewModel.compatibilityRamFilter = nil
            } else {
                viewModel.compatibilityRamFilter = ramGB
            }
        }) {
            HStack(spacing: 4) {
                Text(label)
                    .font(.system(size: 11, weight: isSelected ? .bold : .semibold, design: .rounded))
                    .foregroundColor(.white)
                Text("(\(count))")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundColor(isSelected ? .white : NXTheme.textSecondary)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(
                ZStack {
                    if isSelected {
                        Capsule().fill(NXTheme.primaryGradient)
                    } else {
                        Capsule().fill(NXTheme.glassSurfaceElevated.opacity(0.6))
                    }
                }
            )
            .clipShape(Capsule())
            .overlay(
                Capsule()
                    .strokeBorder(isSelected ? Color.white.opacity(0.4) : NXTheme.borderSubtle, lineWidth: 0.8)
            )
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
        .help(ramGB == nil ? "Show games for all RAM capacities" : "Show games that run on devices with up to \(ramGB!)GB RAM (\(count) games)")
    }
    
    private func nameLetterButton(_ label: String, isSelected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: {
            SoundEffectService.shared.play(.click)
            viewModel.compatibilityFocusArea = .filters
            if isSelected && label != "All" {
                viewModel.compatibilityNameFilter = "All"
            } else {
                action()
            }
        }) {
            Text(label)
                .font(.system(size: 10, weight: isSelected ? .bold : .semibold, design: .rounded))
                .foregroundColor(isSelected ? .white : NXTheme.textSecondary)
                .frame(minWidth: label.count > 1 ? 30 : 20)
                .frame(height: 22)
                .background(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .fill(isSelected ? NXTheme.primary.opacity(0.8) : NXTheme.glassSurfaceElevated.opacity(0.5))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .strokeBorder(
                            isSelected
                                ? LinearGradient(colors: [NXTheme.secondary, NXTheme.primary], startPoint: .topLeading, endPoint: .bottomTrailing)
                                : NXTheme.specularRim,
                            lineWidth: isSelected ? 1 : 0.6
                        )
                )
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
}
