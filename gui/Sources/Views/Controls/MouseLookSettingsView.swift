import SwiftUI

public struct MouseLookSettingsView: View {
    @Binding public var profile: ControllerProfile
    
    public init(profile: Binding<ControllerProfile>) {
        self._profile = profile
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label(L10n.t("controls.mouse_right_stick_title"), systemImage: "cursorarrow.motionlines")
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    
                    Spacer()
                    
                    Toggle("", isOn: $profile.mouseAsRightStick)
                        .toggleStyle(SwitchToggleStyle(tint: NXTheme.primary))
                        .labelsHidden()
                }
                
                Text(L10n.t("controls.mouse_right_stick_desc"))
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
                
                if profile.mouseAsRightStick {
                    Divider().opacity(0.2)
                    
                    VStack(alignment: .leading, spacing: 12) {
                        // Sensitivity Slider (Liquid Glass pattern, range up to 100x, default 5x)
                        HStack {
                            Text(L10n.t("controls.mouse_sensitivity"))
                                .font(.system(size: 11, weight: .medium))
                                .foregroundColor(NXTheme.textPrimary)
                            
                            Spacer()
                            
                            Text(String(format: "%.1fx", profile.mouseSensitivity))
                                .font(.system(size: 11, weight: .bold, design: .monospaced))
                                .foregroundColor(NXTheme.secondary)
                        }
                        
                        NXLiquidGlassSlider(
                            value: $profile.mouseSensitivity,
                            in: 0.5...100.0,
                            step: 0.5,
                            accentColor: NXTheme.primary
                        )
                        .frame(height: 24)
                        
                        Divider().opacity(0.15)
                        
                        // Invert X Axis (Horizontal)
                        Toggle(isOn: $profile.mouseInvertX) {
                            Text(L10n.t("controls.mouse_invert_x"))
                                .font(.system(size: 11, weight: .medium))
                                .foregroundColor(NXTheme.textPrimary)
                        }
                        .toggleStyle(SwitchToggleStyle(tint: NXTheme.primary))
                        
                        Divider().opacity(0.15)
                        
                        // Invert Y Axis (Vertical)
                        Toggle(isOn: $profile.mouseInvertY) {
                            Text(L10n.t("controls.mouse_invert_y"))
                                .font(.system(size: 11, weight: .medium))
                                .foregroundColor(NXTheme.textPrimary)
                        }
                        .toggleStyle(SwitchToggleStyle(tint: NXTheme.primary))
                        
                        Divider().opacity(0.15)
                        
                        // Mode Selection
                        VStack(alignment: .leading, spacing: 6) {
                            Text(L10n.t("controls.mouse_mode_title"))
                                .font(.system(size: 11, weight: .medium))
                                .foregroundColor(NXTheme.textPrimary)
                            
                            Picker("", selection: $profile.mouseRightStickMode) {
                                Text(L10n.t("controls.mouse_mode_lock")).tag("cursorLock")
                                Text(L10n.t("controls.mouse_mode_drag")).tag("rightDrag")
                            }
                            .pickerStyle(.segmented)
                        }
                    }
                    .transition(.opacity.combined(with: .move(edge: .top)))
                }
            }
        }
    }
}
