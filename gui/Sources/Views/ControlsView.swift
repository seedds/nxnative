import SwiftUI
import GameController

public struct ControlsView: View {
    @ObservedObject private var i18n = LocalizationService.shared
    @ObservedObject private var profileManager = InputProfileManager.shared
    @ObservedObject private var controllerService = GameControllerService.shared
    @State private var selectedDeviceIndex: Int = 0 // 0: Controller, 1: Keyboard
    @State private var showNewProfileAlert: Bool = false
    @State private var newProfileName: String = ""
    @State private var activeProfile: ControllerProfile = ControllerProfile.nintendoDefault
    @State private var pressedButtons: Set<SwitchButton> = []
    @State private var monitorTimer: Timer? = nil

    public init() {}

    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                // MARK: - Header & Profile Management
                profileManagementHeader

                // MARK: - Device Mode Selector (Gamepad vs Keyboard)
                deviceSelectorBar

                // MARK: - Main Interactive Mapping Surface
                if selectedDeviceIndex == 0 {
                    // Controller Mapping with Switch Pro Controller in the Center
                    GlassCard {
                        VStack(alignment: .leading, spacing: 14) {
                            HStack {
                                Label(i18n.t("controls.controller_mapping"), systemImage: "gamecontroller.fill")
                                    .font(.system(size: 14, weight: .bold, design: .rounded))
                                    .foregroundColor(NXTheme.textPrimary)

                                Spacer()

                                Text(controllerService.isGamepadConnected ? controllerService.activeControllerName : i18n.t("controls.gamepad_none"))
                                    .font(.system(size: 11, weight: .medium))
                                    .foregroundColor(controllerService.isGamepadConnected ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.textMuted)
                            }

                            Divider().opacity(0.2)

                            ControllerMappingCalloutOverlay(
                                profile: $activeProfile,
                                pressedButtons: pressedButtons
                            )
                        }
                    }
                } else {
                    // Simple Keyboard Mapping View
                    GlassCard {
                        VStack(alignment: .leading, spacing: 14) {
                            HStack {
                                Label(i18n.t("controls.keyboard_mapping"), systemImage: "keyboard.fill")
                                    .font(.system(size: 14, weight: .bold, design: .rounded))
                                    .foregroundColor(NXTheme.textPrimary)

                                Spacer()

                                Text(i18n.t("controls.click_to_rebind"))
                                    .font(.system(size: 11))
                                    .foregroundColor(NXTheme.secondary)
                            }

                            Divider().opacity(0.2)

                            SimpleKeyboardMappingView(profile: $activeProfile)
                        }
                    }
                }

                // MARK: - Analog Stick Deadzone & Calibration (Enhancement 1)
                AnalogDeadzoneTestView(deadzone: $activeProfile.analogDeadzone)
            }
            .padding(20)
        }
        .onAppear {
            activeProfile = profileManager.activeProfile
            setupControllerPressMonitoring()
        }
        .onDisappear {
            monitorTimer?.invalidate()
            monitorTimer = nil
        }
        .onChange(of: profileManager.activeProfileId) { _ in
            activeProfile = profileManager.activeProfile
        }
        .onChange(of: activeProfile) { updated in
            profileManager.updateProfile(updated)
        }
        .sheet(isPresented: $showNewProfileAlert) {
            newProfileSheet
        }
    }

    // MARK: - Profile Management Toolbar
    private var profileManagementHeader: some View {
        HStack(spacing: 12) {
            Text(i18n.t("controls.title"))
                .font(.system(size: 16, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)

            Spacer()

            // Profile Picker Dropdown Input
            Menu {
                ForEach(profileManager.profiles) { prof in
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        profileManager.activeProfileId = prof.id
                        activeProfile = prof
                    }) {
                        if prof.id == profileManager.activeProfileId {
                            Label(prof.name, systemImage: "checkmark")
                        } else {
                            Text(prof.name)
                        }
                    }
                }
                Divider()
                Button(i18n.t("controls.new_profile")) {
                    newProfileName = ""
                    showNewProfileAlert = true
                }
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: "slider.horizontal.2.square")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(NXTheme.secondary)
                    
                    VStack(alignment: .leading, spacing: 1) {
                        Text(i18n.t("controls.profile"))
                            .font(.system(size: 9, weight: .medium))
                            .foregroundColor(NXTheme.textSecondary)
                        Text(profileManager.activeProfile.name)
                            .font(.system(size: 11, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                    }
                    
                    Spacer(minLength: 8)
                    
                    HStack(spacing: 2) {
                        Image(systemName: "chevron.up.chevron.down")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundColor(NXTheme.secondary)
                    }
                    .padding(.horizontal, 5)
                    .padding(.vertical, 3)
                    .background(Color.white.opacity(0.08))
                    .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(NXTheme.glassSurfaceElevated)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .stroke(NXTheme.specularRim, lineWidth: 1.2)
                )
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)

            Button(i18n.t("common.duplicate")) {
                SoundEffectService.shared.play(.click)
                let copy = profileManager.duplicateProfile(activeProfile)
                activeProfile = copy
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .controlSize(.small)

            if !activeProfile.isDefault {
                Button(i18n.t("common.delete")) {
                    SoundEffectService.shared.play(.click)
                    profileManager.deleteProfile(activeProfile)
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
            }

            Button(i18n.t("common.reset")) {
                SoundEffectService.shared.play(.click)
                profileManager.resetActiveProfileToDefault()
                activeProfile = profileManager.activeProfile
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .controlSize(.small)
        }
    }

    // MARK: - Device Mode Selector
    private var deviceSelectorBar: some View {
        HStack(spacing: 12) {
            Picker("", selection: $selectedDeviceIndex) {
                Text(i18n.t("controls.gamepad_tab")).tag(0)
                Text(i18n.t("controls.keyboard_tab")).tag(1)
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 420)

            Spacer()
        }
    }

    // MARK: - New Profile Sheet
    private var newProfileSheet: some View {
        VStack(spacing: 16) {
            Text(i18n.t("controls.create_profile_title"))
                .font(.system(size: 15, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)

            TextField("Profile Name (e.g. Zelda Pro Controls)", text: $newProfileName)
                .textFieldStyle(.roundedBorder)
                .frame(width: 280)

            HStack(spacing: 12) {
                Button(i18n.t("common.cancel")) {
                    showNewProfileAlert = false
                }
                .buttonStyle(NXSecondaryButtonStyle())

                Button(i18n.t("controls.create")) {
                    guard !newProfileName.trimmingCharacters(in: .whitespaces).isEmpty else { return }
                    let created = profileManager.createProfile(name: newProfileName, basedOn: activeProfile)
                    activeProfile = created
                    showNewProfileAlert = false
                }
                .buttonStyle(NXPrimaryButtonStyle())
                .disabled(newProfileName.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(24)
        .frame(width: 340, height: 160)
        .background(NXTheme.canvasObsidian)
    }

    private func setupControllerPressMonitoring() {
        monitorTimer?.invalidate()
        monitorTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [self] _ in
            if let pad = GCController.controllers().first?.extendedGamepad {
                var pressed = Set<SwitchButton>()
                for (elemName, switchBtn) in activeProfile.gamepadBindings {
                    if isGamepadElementPressed(pad: pad, element: elemName) {
                        pressed.insert(switchBtn)
                    }
                }
                self.pressedButtons = pressed
            } else {
                self.pressedButtons = []
            }
        }
    }

    private func isGamepadElementPressed(pad: GCExtendedGamepad, element: String) -> Bool {
        switch element {
        case "buttonA": return pad.buttonA.isPressed
        case "buttonB": return pad.buttonB.isPressed
        case "buttonX": return pad.buttonX.isPressed
        case "buttonY": return pad.buttonY.isPressed
        case "leftShoulder": return pad.leftShoulder.isPressed
        case "rightShoulder": return pad.rightShoulder.isPressed
        case "leftTrigger": return pad.leftTrigger.isPressed
        case "rightTrigger": return pad.rightTrigger.isPressed
        case "buttonMenu": return pad.buttonMenu.isPressed
        case "buttonOptions": return pad.buttonOptions?.isPressed ?? false
        case "leftThumbstickButton": return pad.leftThumbstickButton?.isPressed ?? false
        case "rightThumbstickButton": return pad.rightThumbstickButton?.isPressed ?? false
        case "dpad.up": return pad.dpad.up.isPressed
        case "dpad.down": return pad.dpad.down.isPressed
        case "dpad.left": return pad.dpad.left.isPressed
        case "dpad.right": return pad.dpad.right.isPressed
        default: return false
        }
    }
}
