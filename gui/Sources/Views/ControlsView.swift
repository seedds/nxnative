import SwiftUI
import GameController

public struct ControlsView: View {
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
                                Label("Controller Button Mapping", systemImage: "gamecontroller.fill")
                                    .font(.system(size: 14, weight: .bold, design: .rounded))
                                    .foregroundColor(NXTheme.textPrimary)

                                Spacer()

                                Text(controllerService.isGamepadConnected ? controllerService.activeControllerName : "No Gamepad Connected (Using Virtual Pro Controller)")
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
                                Label("Simple Keyboard Mapping", systemImage: "keyboard.fill")
                                    .font(.system(size: 14, weight: .bold, design: .rounded))
                                    .foregroundColor(NXTheme.textPrimary)

                                Spacer()

                                Text("Click any key button to rebind")
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
            Image(systemName: "gamecontroller.fill")
                .symbolRenderingMode(.hierarchical)
                .font(.system(size: 28))
                .foregroundColor(NXTheme.secondary)

            VStack(alignment: .leading, spacing: 2) {
                Text("Controls & Gamepad Mapping")
                    .font(.system(size: 16, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Configure custom button bindings and profiles for every controller and keyboard.")
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
            }

            Spacer()

            // Profile Picker
            Menu {
                ForEach(profileManager.profiles) { prof in
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        profileManager.activeProfileId = prof.id
                    }) {
                        if prof.id == profileManager.activeProfileId {
                            Label(prof.name, systemImage: "checkmark")
                        } else {
                            Text(prof.name)
                        }
                    }
                }
                Divider()
                Button("New Profile...") {
                    newProfileName = ""
                    showNewProfileAlert = true
                }
            } label: {
                HStack(spacing: 6) {
                    Text("Profile:")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(NXTheme.textSecondary)
                    Text(activeProfile.name)
                        .font(.system(size: 11, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    Image(systemName: "chevron.down")
                        .font(.system(size: 9, weight: .bold))
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(NXTheme.glassSurfaceElevated)
                .cornerRadius(8)
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(NXTheme.specularRim, lineWidth: 0.8))
            }
            .menuStyle(.borderlessButton)

            Button("Duplicate") {
                SoundEffectService.shared.play(.click)
                let copy = profileManager.duplicateProfile(activeProfile)
                activeProfile = copy
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .controlSize(.small)

            if !activeProfile.isDefault {
                Button("Delete") {
                    SoundEffectService.shared.play(.click)
                    profileManager.deleteProfile(activeProfile)
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
            }

            Button("Reset") {
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
                Text("Gamepad Controller (Switch Pro / Xbox / PS)").tag(0)
                Text("Mac Keyboard & Mouse").tag(1)
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 420)

            Spacer()
        }
    }

    // MARK: - New Profile Sheet
    private var newProfileSheet: some View {
        VStack(spacing: 16) {
            Text("Create New Controller Profile")
                .font(.system(size: 15, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)

            TextField("Profile Name (e.g. Zelda Pro Controls)", text: $newProfileName)
                .textFieldStyle(.roundedBorder)
                .frame(width: 280)

            HStack(spacing: 12) {
                Button("Cancel") {
                    showNewProfileAlert = false
                }
                .buttonStyle(NXSecondaryButtonStyle())

                Button("Create") {
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
