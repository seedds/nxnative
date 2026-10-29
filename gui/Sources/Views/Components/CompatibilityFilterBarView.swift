import SwiftUI

public struct CompatibilityFilterBarView: View {
    @ObservedObject public var viewModel: AppViewModel
    @Namespace private var ramFilterNamespace
    @Namespace private var chipFilterNamespace
    @Namespace private var alphaFilterNamespace
    
    private let alphabet = (65...90).map { String(UnicodeScalar($0)) }
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var hasActiveFilter: Bool {
        viewModel.compatibilityRamFilter != nil ||
        viewModel.compatibilityChipFilter != nil ||
        viewModel.compatibilityNameFilter != "All"
    }
    
    public var body: some View {
        VStack(spacing: 8) {
            // Row 1: Sliding Liquid Glass Hardware Filters (Mac RAM + iPhone Chip Tier)
            ScrollView(.horizontal, showsIndicators: false) {
                NXGlassContainer(spacing: 8) {
                    HStack(spacing: 12) {
                        // Mac RAM Filter Group
                        HStack(spacing: 6) {
                            HStack(spacing: 4) {
                                Image(systemName: "memorychip")
                                    .font(.system(size: 11, weight: .semibold))
                                    .foregroundColor(NXTheme.secondary)
                                Text(viewModel.tr("compatibility.ram_filter"))
                                    .font(.system(size: 11, weight: .bold, design: .rounded))
                                    .foregroundColor(.white)
                            }
                            
                            HStack(spacing: 5) {
                                ramFilterPill(label: viewModel.tr("compatibility.all"), ramGB: nil, count: viewModel.compatibleGames.count)
                                ramFilterPill(label: "4GB", ramGB: 4, count: viewModel.compatibleGames.filter { $0.minRamGB <= 4 }.count)
                                ramFilterPill(label: "6GB", ramGB: 6, count: viewModel.compatibleGames.filter { $0.minRamGB <= 6 }.count)
                                ramFilterPill(label: "8GB", ramGB: 8, count: viewModel.compatibleGames.filter { $0.minRamGB <= 8 }.count)
                                ramFilterPill(label: "16GB", ramGB: 16, count: viewModel.compatibleGames.filter { $0.minRamGB <= 16 }.count)
                            }
                            .background {
                                NXLiquidSlidingPill(in: Capsule())
                                    .matchedGeometryEffect(
                                        id: viewModel.compatibilityRamFilter ?? -1,
                                        in: ramFilterNamespace,
                                        properties: .frame,
                                        anchor: .center,
                                        isSource: false
                                    )
                                    .animation(.spring(response: 0.34, dampingFraction: 0.76), value: viewModel.compatibilityRamFilter)
                            }
                        }
                        
                        Rectangle()
                            .fill(Color.white.opacity(0.14))
                            .frame(width: 1, height: 18)
                        
                        // iPhone Hardware Chip Filter Group
                        HStack(spacing: 6) {
                            HStack(spacing: 4) {
                                Image(systemName: "iphone")
                                    .font(.system(size: 11, weight: .semibold))
                                    .foregroundColor(NXTheme.secondary)
                                Text(viewModel.tr("compatibility.iphone_chip_filter"))
                                    .font(.system(size: 11, weight: .bold, design: .rounded))
                                    .foregroundColor(.white)
                            }
                            
                            HStack(spacing: 5) {
                                chipFilterPill(label: viewModel.tr("compatibility.all"), chip: nil, count: viewModel.compatibleGames.count)
                                ForEach(IphoneChipTier.allCases) { tier in
                                    let count = viewModel.compatibleGames.filter { $0.minChipTier <= tier }.count
                                    chipFilterPill(label: tier.rawValue, chip: tier, count: count)
                                }
                            }
                            .background {
                                NXLiquidSlidingPill(in: Capsule())
                                    .matchedGeometryEffect(
                                        id: viewModel.compatibilityChipFilter?.rawValue ?? "AllChips",
                                        in: chipFilterNamespace,
                                        properties: .frame,
                                        anchor: .center,
                                        isSource: false
                                    )
                                    .animation(.spring(response: 0.34, dampingFraction: 0.76), value: viewModel.compatibilityChipFilter)
                            }
                        }
                        
                        if hasActiveFilter {
                            Button(action: {
                                SoundEffectService.shared.play(.click)
                                withAnimation(.spring(response: 0.34, dampingFraction: 0.76)) {
                                    viewModel.compatibilityRamFilter = nil
                                    viewModel.compatibilityChipFilter = nil
                                    viewModel.compatibilityNameFilter = "All"
                                }
                            }) {
                                HStack(spacing: 4) {
                                    Image(systemName: "xmark.circle.fill")
                                        .font(.system(size: 10))
                                    Text(viewModel.tr("common.reset"))
                                        .font(.system(size: 10, weight: .semibold))
                                }
                                .foregroundColor(.white)
                                .padding(.horizontal, 9)
                                .padding(.vertical, 4.5)
                                .contentShape(Capsule())
                                .nxGlassInteractive(
                                    in: Capsule(),
                                    tint: NXTheme.tertiary.opacity(0.50),
                                    fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.6)
                                )
                                .nxGlassTransition(.materialize)
                            }
                            .buttonStyle(NXPressableButtonStyle())
                            .nxNoFocusRing()
                        }
                    }
                    .padding(.horizontal, 16)
                }
            }
            
            // Row 2: Centered [New | All | A-Z] Filter Buttons (Single persistent sliding Liquid Glass droplet)
            GeometryReader { geo in
                ScrollView(.horizontal, showsIndicators: false) {
                    NXGlassContainer(spacing: 4) {
                        HStack(spacing: 3) {
                            nameLetterButton(
                                viewModel.tr("compatibility.new"),
                                icon: "sparkles",
                                filterKey: "New",
                                isSelected: viewModel.compatibilityNameFilter == "New"
                            ) {
                                viewModel.compatibilityNameFilter = "New"
                            }
                            
                            nameLetterButton(
                                viewModel.tr("compatibility.all"),
                                filterKey: "All",
                                isSelected: viewModel.compatibilityNameFilter == "All"
                            ) {
                                viewModel.compatibilityNameFilter = "All"
                            }
                            
                            ForEach(alphabet, id: \.self) { letter in
                                nameLetterButton(letter, filterKey: letter, isSelected: viewModel.compatibilityNameFilter == letter) {
                                    viewModel.compatibilityNameFilter = letter
                                }
                            }
                        }
                        .background {
                            NXLiquidSlidingPill(in: RoundedRectangle(cornerRadius: 6, style: .continuous))
                                .matchedGeometryEffect(
                                    id: viewModel.compatibilityNameFilter,
                                    in: alphaFilterNamespace,
                                    properties: .frame,
                                    anchor: .center,
                                    isSource: false
                                )
                                .animation(.spring(response: 0.32, dampingFraction: 0.76), value: viewModel.compatibilityNameFilter)
                        }
                        .padding(.horizontal, 16)
                        .frame(minWidth: geo.size.width, alignment: .center)
                    }
                }
            }
            .frame(height: 24)
        }
        .padding(.vertical, 8)
    }
    
    private func ramFilterPill(label: String, ramGB: Int?, count: Int) -> some View {
        let isSelected = viewModel.compatibilityRamFilter == ramGB
        let pillID: Int = ramGB ?? -1
        return Button(action: {
            SoundEffectService.shared.play(.click)
            withAnimation(.spring(response: 0.34, dampingFraction: 0.76)) {
                viewModel.compatibilityFocusArea = .filters
                if viewModel.compatibilityRamFilter == ramGB && ramGB != nil {
                    viewModel.compatibilityRamFilter = nil
                } else {
                    viewModel.compatibilityRamFilter = ramGB
                }
            }
        }) {
            HStack(spacing: 3) {
                Text(label)
                    .font(.system(size: 10.5, weight: isSelected ? .bold : .semibold, design: .rounded))
                    .foregroundColor(.white)
                Text("(\(count))")
                    .font(.system(size: 9.5, weight: .semibold, design: .monospaced))
                    .foregroundColor(.white)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(
                Capsule()
                    .fill(isSelected ? Color.clear : Color.white.opacity(0.06))
            )
            .overlay(
                Capsule()
                    .strokeBorder(isSelected ? Color.clear : Color.white.opacity(0.10), lineWidth: 0.6)
            )
            .contentShape(Capsule())
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
        .matchedGeometryEffect(id: pillID, in: ramFilterNamespace, isSource: true)
        .help(ramGB == nil ? "Show games for all RAM capacities" : "Show games that run on devices with up to \(ramGB!)GB RAM (\(count) games)")
    }
    
    private func chipFilterPill(label: String, chip: IphoneChipTier?, count: Int) -> some View {
        let isSelected = viewModel.compatibilityChipFilter == chip
        let pillID: String = chip?.rawValue ?? "AllChips"
        return Button(action: {
            SoundEffectService.shared.play(.click)
            withAnimation(.spring(response: 0.34, dampingFraction: 0.76)) {
                viewModel.compatibilityFocusArea = .filters
                if viewModel.compatibilityChipFilter == chip && chip != nil {
                    viewModel.compatibilityChipFilter = nil
                } else {
                    viewModel.compatibilityChipFilter = chip
                }
            }
        }) {
            HStack(spacing: 3) {
                Text(label)
                    .font(.system(size: 10.5, weight: isSelected ? .bold : .semibold, design: .rounded))
                    .foregroundColor(.white)
                Text("(\(count))")
                    .font(.system(size: 9.5, weight: .semibold, design: .monospaced))
                    .foregroundColor(.white)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(
                Capsule()
                    .fill(isSelected ? Color.clear : Color.white.opacity(0.06))
            )
            .overlay(
                Capsule()
                    .strokeBorder(isSelected ? Color.clear : Color.white.opacity(0.10), lineWidth: 0.6)
            )
            .contentShape(Capsule())
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
        .matchedGeometryEffect(id: pillID, in: chipFilterNamespace, isSource: true)
        .help(chip?.deviceSummary ?? "Show games for all iPhone / iOS chips")
    }
    
    private func nameLetterButton(
        _ label: String,
        icon: String? = nil,
        filterKey: String,
        isSelected: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: {
            SoundEffectService.shared.play(.click)
            withAnimation(.spring(response: 0.32, dampingFraction: 0.76)) {
                viewModel.compatibilityFocusArea = .filters
                if isSelected && filterKey != "All" {
                    viewModel.compatibilityNameFilter = "All"
                } else {
                    action()
                }
            }
        }) {
            HStack(spacing: 3) {
                if let icon {
                    Image(systemName: icon)
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.white)
                }
                Text(label)
                    .font(.system(size: 10, weight: isSelected ? .bold : .semibold, design: .rounded))
                    .foregroundColor(.white)
            }
            .padding(.horizontal, label.count > 1 ? 7 : 0)
            .frame(minWidth: label.count > 1 ? 34 : 20)
            .frame(height: 22)
            .background(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(isSelected ? Color.clear : Color.white.opacity(0.05))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .strokeBorder(isSelected ? Color.clear : Color.white.opacity(0.08), lineWidth: 0.5)
            )
            .contentShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
        .matchedGeometryEffect(id: filterKey, in: alphaFilterNamespace, isSource: true)
    }
}
