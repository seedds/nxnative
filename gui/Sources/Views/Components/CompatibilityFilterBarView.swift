import SwiftUI

public struct CompatibilityFilterBarView: View {
    @ObservedObject public var viewModel: AppViewModel
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
            // Row 1: Dropdown Hardware Filters (Mac RAM + iPhone Chip Tier)
            GeometryReader { geo in
                ScrollView(.horizontal, showsIndicators: false) {
                    NXGlassContainer(spacing: 8) {
                        HStack(spacing: 12) {
                            ramDropdownMenu
                            
                            Rectangle()
                                .fill(Color.white.opacity(0.14))
                                .frame(width: 1, height: 18)
                            
                            chipDropdownMenu
                            
                            if hasActiveFilter {
                                resetButton
                            }
                        }
                        .padding(.horizontal, 14)
                        .padding(.vertical, 3)
                        .frame(minWidth: geo.size.width, alignment: .center)
                    }
                }
            }
            .frame(height: 30)
            
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
    
    // MARK: - RAM Dropdown
    private var ramDropdownMenu: some View {
        let isFiltered = viewModel.compatibilityRamFilter != nil
        // When filtered, substitutes "RAM" with the selected option (e.g. "8GB").
        // When not filtered, displays default "RAM".
        let displayText: String = {
            if let ram = viewModel.compatibilityRamFilter {
                return "\(ram)GB"
            } else {
                return viewModel.tr("compatibility.ram_filter").replacingOccurrences(of: ":", with: "").trimmingCharacters(in: .whitespaces)
            }
        }()
        
        return Menu {
            Picker("RAM", selection: Binding(
                get: { viewModel.compatibilityRamFilter ?? -1 },
                set: { newRam in
                    SoundEffectService.shared.play(.click)
                    withAnimation(.spring(response: 0.34, dampingFraction: 0.76)) {
                        viewModel.compatibilityFocusArea = .filters
                        viewModel.compatibilityRamFilter = (newRam == -1) ? nil : newRam
                    }
                }
            )) {
                Text("\(viewModel.tr("compatibility.all")) (\(viewModel.compatibleGames.count))")
                    .tag(-1)
                
                ForEach([4, 6, 8, 16], id: \.self) { ram in
                    let count = viewModel.compatibleGames.filter { $0.minRamGB <= ram }.count
                    Text("\(ram)GB (\(count))")
                        .tag(ram)
                }
            }
            .pickerStyle(.inline)
        } label: {
            ramMenuLabel(isFiltered: isFiltered, displayText: displayText)
        }
        .menuStyle(.button)
        .buttonStyle(NXPressableButtonStyle())
        .menuIndicator(.hidden)
        .fixedSize()
        .nxNoFocusRing()
        .help(viewModel.compatibilityRamFilter == nil ? "Filter games by minimum RAM" : "Filtered: up to \(viewModel.compatibilityRamFilter!)GB RAM")
    }
    
    private func ramMenuLabel(isFiltered: Bool, displayText: String) -> some View {
        let intensity = viewModel.liquidGlassIntensity
        let buttonBgOpacity = isFiltered
            ? (0.18 + 0.12 * intensity)
            : (0.04 + 0.05 * (1.0 - intensity))
        let borderOpacity = isFiltered
            ? (0.35 + 0.15 * intensity)
            : (0.08 + 0.10 * intensity)
        
        return HStack(spacing: 6) {
            Image(systemName: "memorychip")
                .font(.system(size: 11, weight: .semibold))
                .foregroundColor(isFiltered ? NXTheme.primary : NXTheme.secondary)
            
            Text(displayText)
                .font(.system(size: 11, weight: isFiltered ? .bold : .semibold, design: .rounded))
                .foregroundColor(isFiltered ? NXTheme.primary : .white)
            
            Image(systemName: "chevron.up.chevron.down")
                .font(.system(size: 8, weight: .bold))
                .foregroundColor(isFiltered ? NXTheme.primary.opacity(0.85) : .white.opacity(0.65))
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(
            Capsule()
                .fill(isFiltered ? NXTheme.primary.opacity(buttonBgOpacity) : Color.white.opacity(buttonBgOpacity))
        )
        .overlay(
            Capsule()
                .strokeBorder(
                    isFiltered
                        ? NXTheme.primary.opacity(borderOpacity)
                        : Color.white.opacity(borderOpacity),
                    lineWidth: 0.75
                )
        )
        .contentShape(Capsule())
        .nxGlassInteractive(
            in: Capsule(),
            tint: isFiltered ? NXTheme.primary.opacity(0.35) : nil,
            fallbackFill: Color.clear
        )
    }
    
    // MARK: - iPhone Chip Dropdown
    private var chipDropdownMenu: some View {
        let isFiltered = viewModel.compatibilityChipFilter != nil
        // When filtered, substitutes "Chip iPhone" with the selected chip tier (e.g. "A17 Pro").
        // When not filtered, displays default "Chip iPhone".
        let displayText: String = {
            if let chip = viewModel.compatibilityChipFilter {
                return chip.rawValue
            } else {
                return viewModel.tr("compatibility.iphone_chip_filter").replacingOccurrences(of: ":", with: "").trimmingCharacters(in: .whitespaces)
            }
        }()
        
        return Menu {
            Picker("Chip", selection: Binding(
                get: { viewModel.compatibilityChipFilter?.rawValue ?? "All" },
                set: { val in
                    SoundEffectService.shared.play(.click)
                    withAnimation(.spring(response: 0.34, dampingFraction: 0.76)) {
                        viewModel.compatibilityFocusArea = .filters
                        if val == "All" {
                            viewModel.compatibilityChipFilter = nil
                        } else {
                            viewModel.compatibilityChipFilter = IphoneChipTier(rawValue: val)
                        }
                    }
                }
            )) {
                Text("\(viewModel.tr("compatibility.all")) (\(viewModel.compatibleGames.count))")
                    .tag("All")
                
                ForEach(IphoneChipTier.allCases) { tier in
                    let count = viewModel.compatibleGames.filter { $0.minChipTier <= tier }.count
                    Text("\(tier.rawValue) — \(tier.deviceSummary) (\(count))")
                        .tag(tier.rawValue)
                }
            }
            .pickerStyle(.inline)
        } label: {
            chipMenuLabel(isFiltered: isFiltered, displayText: displayText)
        }
        .menuStyle(.button)
        .buttonStyle(NXPressableButtonStyle())
        .menuIndicator(.hidden)
        .fixedSize()
        .nxNoFocusRing()
        .help(viewModel.compatibilityChipFilter?.deviceSummary ?? "Filter games by minimum iPhone chip tier")
    }
    
    private func chipMenuLabel(isFiltered: Bool, displayText: String) -> some View {
        let intensity = viewModel.liquidGlassIntensity
        let buttonBgOpacity = isFiltered
            ? (0.18 + 0.12 * intensity)
            : (0.04 + 0.05 * (1.0 - intensity))
        let borderOpacity = isFiltered
            ? (0.35 + 0.15 * intensity)
            : (0.08 + 0.10 * intensity)
        
        return HStack(spacing: 6) {
            Image(systemName: "iphone")
                .font(.system(size: 11, weight: .semibold))
                .foregroundColor(isFiltered ? NXTheme.primary : NXTheme.secondary)
            
            Text(displayText)
                .font(.system(size: 11, weight: isFiltered ? .bold : .semibold, design: .rounded))
                .foregroundColor(isFiltered ? NXTheme.primary : .white)
            
            Image(systemName: "chevron.up.chevron.down")
                .font(.system(size: 8, weight: .bold))
                .foregroundColor(isFiltered ? NXTheme.primary.opacity(0.85) : .white.opacity(0.65))
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(
            Capsule()
                .fill(isFiltered ? NXTheme.primary.opacity(buttonBgOpacity) : Color.white.opacity(buttonBgOpacity))
        )
        .overlay(
            Capsule()
                .strokeBorder(
                    isFiltered
                        ? NXTheme.primary.opacity(borderOpacity)
                        : Color.white.opacity(borderOpacity),
                    lineWidth: 0.75
                )
        )
        .contentShape(Capsule())
        .nxGlassInteractive(
            in: Capsule(),
            tint: isFiltered ? NXTheme.primary.opacity(0.35) : nil,
            fallbackFill: Color.clear
        )
    }
    
    // MARK: - Reset Button
    private var resetButton: some View {
        let intensity = viewModel.liquidGlassIntensity
        return Button(action: {
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
            .background(
                Capsule()
                    .fill(NXTheme.tertiary.opacity(0.25 + 0.15 * intensity))
            )
            .overlay(
                Capsule()
                    .strokeBorder(NXTheme.tertiary.opacity(0.40 + 0.20 * intensity), lineWidth: 0.75)
            )
            .contentShape(Capsule())
            .nxGlassInteractive(
                in: Capsule(),
                tint: NXTheme.tertiary.opacity(0.40),
                fallbackFill: Color.clear
            )
            .nxGlassTransition(.materialize)
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
    }
    
    // MARK: - Alphabet Buttons
    private func nameLetterButton(
        _ label: String,
        icon: String? = nil,
        filterKey: String,
        isSelected: Bool,
        action: @escaping () -> Void
    ) -> some View {
        let intensity = viewModel.liquidGlassIntensity
        return Button(action: {
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
                    .fill(isSelected ? Color.clear : Color.white.opacity(0.04 + 0.04 * (1.0 - intensity)))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .strokeBorder(isSelected ? Color.clear : Color.white.opacity(0.06 + 0.06 * intensity), lineWidth: 0.5)
            )
            .contentShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
        .matchedGeometryEffect(id: filterKey, in: alphaFilterNamespace, isSource: true)
    }
}
