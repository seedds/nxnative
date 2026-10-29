import SwiftUI
import GameController

public struct ControlsView: View {
    @ObservedObject private var i18n = LocalizationService.shared
    @ObservedObject private var profileManager = InputProfileManager.shared
    @ObservedObject private var controllerService = GameControllerService.shared
    @Environment(\.nxLiquidGlass) private var glassEnv
    @State private var selectedDeviceIndex: Int = 0 // 0: Controller, 1: Keyboard
    @State private var showNewProfileAlert: Bool = false
    @State private var newProfileName: String = ""
    @State private var activeProfile: ControllerProfile = ControllerProfile.nintendoDefault
    @State private var pressedButtons: Set<SwitchButton> = []
    @State private var monitorTimer: Timer? = nil

    public init() {
        let hasGamepad = GameControllerService.shared.isGamepadConnected
        _selectedDeviceIndex = State(initialValue: hasGamepad ? 0 : 1)
        let current = InputProfileManager.shared.activeProfile
        _activeProfile = State(initialValue: current)
    }

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
        .nxScrollEdgeEffect(edges: [.top, .bottom])
        .onAppear {
            if !controllerService.isGamepadConnected {
                selectedDeviceIndex = 1
            }
            activeProfile = profileManager.activeProfile
            if selectedDeviceIndex == 0 {
                setupControllerPressMonitoring()
            }
        }
        .onDisappear {
            stopControllerPressMonitoring()
            profileManager.updateProfile(activeProfile)
            profileManager.flushAllSettings()
        }
        .onChange(of: controllerService.isGamepadConnected) { isConnected in
            if !isConnected && selectedDeviceIndex == 0 {
                withAnimation(.spring(response: 0.34, dampingFraction: 0.76)) {
                    selectedDeviceIndex = 1
                }
            }
        }
        .onChange(of: selectedDeviceIndex) { newIndex in
            if newIndex == 0 {
                setupControllerPressMonitoring()
            } else {
                stopControllerPressMonitoring()
            }
        }
        .onChange(of: profileManager.activeProfileId) { _ in
            activeProfile = profileManager.activeProfile
        }
        .onChange(of: activeProfile) { updated in
            profileManager.updateProfile(updated)
        }
        .sheet(isPresented: $showNewProfileAlert) {
            newProfileSheet
                .environment(\.nxLiquidGlass, glassEnv)
        }
    }

    // MARK: - Profile Management Toolbar
    private var profileManagementHeader: some View {
        HStack(spacing: 12) {
            Text(i18n.t("controls.title"))
                .font(.system(size: 16, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)

            Spacer()

            NXGlassContainer(spacing: 8) {
                HStack(spacing: 8) {
                    // Profile Selector Pill: displays the selected profile name directly in place of "Profile"
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
                            Image(systemName: "gamecontroller.fill")
                                .font(.system(size: 11, weight: .semibold))
                                .foregroundColor(.white)

                            Text(activeProfile.name)
                                .font(.system(size: 12, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                                .lineLimit(1)

                            Image(systemName: "chevron.up.chevron.down")
                                .font(.system(size: 9, weight: .bold))
                                .foregroundColor(.white.opacity(0.85))
                        }
                        .padding(.horizontal, 12)
                        .padding(.vertical, 7)
                        .contentShape(Capsule())
                        .nxGlassInteractive(
                            in: Capsule(),
                            fallbackFill: NXTheme.glassSurfaceElevated
                        )
                    }
                    .menuStyle(.button)
                    .buttonStyle(NXPressableButtonStyle())
                    .menuIndicator(.hidden)
                    .fixedSize()
                    .nxNoFocusRing()

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
                        .nxGlassTransition(.materialize)
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
        }
    }

    // MARK: - Device Mode Selector
    private var deviceSelectorBar: some View {
        HStack(spacing: 12) {
            NXLiquidSegmentedPicker(
                selection: $selectedDeviceIndex,
                options: [
                    NXSegmentOption(value: 0, title: i18n.t("controls.gamepad_tab"), icon: "gamecontroller.fill"),
                    NXSegmentOption(value: 1, title: i18n.t("controls.keyboard_tab"), icon: "keyboard.fill")
                ]
            )
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

            NXLiquidGlassTextField("Profile Name (e.g. Zelda Pro Controls)", text: $newProfileName)
                .frame(width: 290)

            NXGlassContainer(spacing: 12) {
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
        }
        .padding(24)
        .frame(width: 340, height: 160)
        .nxSheetSurface()
    }

    private func setupControllerPressMonitoring() {
        stopControllerPressMonitoring()
        guard controllerService.isGamepadConnected else {
            self.pressedButtons = []
            return
        }
        monitorTimer = Timer.scheduledTimer(withTimeInterval: 0.08, repeats: true) { [self] _ in
            guard NSApplication.shared.isActive && !MenuBarManager.isMinimized else { return }
            guard controllerService.isGamepadConnected else {
                if !self.pressedButtons.isEmpty { self.pressedButtons = [] }
                return
            }
            if let pad = GCController.controllers().first?.extendedGamepad {
                var pressed = Set<SwitchButton>()
                for (elemName, switchBtn) in activeProfile.gamepadBindings {
                    if isGamepadElementPressed(pad: pad, element: elemName) {
                        pressed.insert(switchBtn)
                    }
                }
                if self.pressedButtons != pressed {
                    self.pressedButtons = pressed
                }
            } else if !self.pressedButtons.isEmpty {
                self.pressedButtons = []
            }
        }
    }

    private func stopControllerPressMonitoring() {
        monitorTimer?.invalidate()
        monitorTimer = nil
        if !pressedButtons.isEmpty {
            pressedButtons = []
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
