import SwiftUI

public struct ControlsView: View {
    public init() {}
    
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // MARK: - Header
                HStack(alignment: .top, spacing: 14) {
                    Image(systemName: "gamecontroller.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 36))
                        .foregroundColor(NXTheme.secondary)
                    
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Controls & Keyboard Mapping")
                            .font(.system(size: 18, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        Text("Games ported by NxNative Launcher simultaneously support your Mac keyboard and any USB or Bluetooth gamepad with instant plug-and-play recognition.")
                            .font(.system(size: 12))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                }
                .padding(.bottom, 4)
                
                // MARK: - Section 1: Visual Quick Reference Table
                GlassCard {
                    VStack(alignment: .leading, spacing: 14) {
                        HStack {
                            Label("Default Keyboard Mapping (Player 1)", systemImage: "keyboard.fill")
                                .symbolRenderingMode(.hierarchical)
                                .font(.system(size: 14, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textPrimary)
                            Spacer()
                            Text("Nintendo Switch Layout")
                                .font(.system(size: 11, weight: .semibold))
                                .foregroundColor(NXTheme.secondary)
                        }
                        
                        Divider().opacity(0.2)
                        
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 340, maximum: .infinity), spacing: 12)], spacing: 12) {
                            // Left Column: Movement & Actions
                            VStack(spacing: 10) {
                                controlRow(switchButton: "Left Stick (Move)", keyDesc: "W  A  S  D", detail: "360° Omnidirectional Movement")
                                controlRow(switchButton: "Left Stick Click (L3)", keyDesc: "F", detail: "Press Left Analog Stick")
                                controlRow(switchButton: "Right Stick Click (R3)", keyDesc: "G", detail: "Press Right Analog Stick")
                                controlRow(switchButton: "A Button (Confirm)", keyDesc: "J", detail: "Right face button (Nintendo layout)")
                                controlRow(switchButton: "B Button (Cancel / Jump)", keyDesc: "K", detail: "Bottom face button (Nintendo layout)")
                                controlRow(switchButton: "X Button (Menu / Special)", keyDesc: "U", detail: "Top face button (Nintendo layout)")
                                controlRow(switchButton: "Y Button (Secondary Action)", keyDesc: "I", detail: "Left face button (Nintendo layout)")
                            }
                            
                            // Right Column: Triggers, D-Pad & System
                            VStack(spacing: 10) {
                                controlRow(switchButton: "Left Bumper (L)", keyDesc: "Q", detail: "Top Left Shoulder")
                                controlRow(switchButton: "Right Bumper (R)", keyDesc: "E", detail: "Top Right Shoulder")
                                controlRow(switchButton: "Left Trigger (ZL)", keyDesc: "Z", detail: "Analog ZL Trigger")
                                controlRow(switchButton: "Right Trigger (ZR)", keyDesc: "C", detail: "Analog ZR Trigger")
                                controlRow(switchButton: "Directional Pad (D-Pad)", keyDesc: "↑  ↓  ←  →", detail: "Keyboard Arrow Keys")
                                controlRow(switchButton: "+ Button (Plus / Start)", keyDesc: "Return (Enter)", detail: "Pause / Main Menu")
                                controlRow(switchButton: "− Button (Minus / Select)", keyDesc: "Tab", detail: "Map / Inventory")
                                controlRow(switchButton: "Shake Controller", keyDesc: "Spacebar", detail: "Motion Shake (Mario Galaxy / Odyssey)")
                            }
                        }
                    }
                }
                
                // MARK: - Section 2: Mouse Touchscreen & Gestures
                GlassCard {
                    HStack(alignment: .top, spacing: 14) {
                        Image(systemName: "cursorarrow.rays")
                            .font(.system(size: 28))
                            .foregroundColor(NXTheme.primary)
                            .frame(width: 36)
                        
                        VStack(alignment: .leading, spacing: 5) {
                            Text("Touchscreen Emulation (Mouse)")
                                .font(.system(size: 13, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textPrimary)
                            Text("In games featuring touch controls or pointer navigation (e.g., Captain Toad, Super Mario Galaxy, and interactive menus):")
                                .font(.system(size: 12))
                                .foregroundColor(NXTheme.textSecondary)
                            
                            HStack(spacing: 8) {
                                Text("Left-Click and Drag")
                                    .font(.system(size: 11, weight: .semibold, design: .monospaced))
                                    .padding(.horizontal, 8)
                                    .padding(.vertical, 4)
                                    .background(NXTheme.primary.opacity(0.18))
                                    .foregroundColor(NXTheme.secondary)
                                    .cornerRadius(6)
                                Text("→ Simulates capacitive touch gestures on the Switch screen")
                                    .font(.system(size: 11))
                                    .foregroundColor(NXTheme.textMuted)
                            }
                            .padding(.top, 2)
                        }
                    }
                }
                
                // MARK: - Section 3: Physical Gamepads & Multiplayer Co-op
                GlassCard {
                    VStack(alignment: .leading, spacing: 12) {
                        HStack {
                            Label("Physical Gamepads & Bluetooth Controllers", systemImage: "cable.connector")
                                .symbolRenderingMode(.hierarchical)
                                .font(.system(size: 13, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textPrimary)
                            Spacer()
                            Text("Plug & Play")
                                .font(.system(size: 11, weight: .bold))
                                .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                        }
                        
                        Text("Connect via Bluetooth or USB-C before or during gameplay. NxNative Launcher automatically maps standard inputs:")
                            .font(.system(size: 12))
                            .foregroundColor(NXTheme.textSecondary)
                        
                        VStack(alignment: .leading, spacing: 8) {
                            bulletPoint("PlayStation DualSense / DualShock 4: Full native haptics and analog triggers.")
                            bulletPoint("Xbox Wireless Controller: Immediate ABXY and trigger rumble support.")
                            bulletPoint("Nintendo Switch Pro Controller: Exact 1:1 button layout matching original hardware.")
                            bulletPoint("Nintendo Switch Joy-Cons: Supported as single horizontal pads or paired Joy-Con pair.")
                        }
                    }
                }
            }
            .padding(20)
        }
    }
    
    private func controlRow(switchButton: String, keyDesc: String, detail: String) -> some View {
        HStack(spacing: 10) {
            Text(keyDesc)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(NXTheme.textPrimary)
                .padding(.horizontal, 8)
                .padding(.vertical, 5)
                .background(NXTheme.glassSurfaceElevated)
                .cornerRadius(6)
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(NXTheme.specularRim, lineWidth: 0.8)
                )
                .shadow(color: Color.black.opacity(0.3), radius: 3, x: 0, y: 1)
                .frame(minWidth: 100, alignment: .center)
            
            VStack(alignment: .leading, spacing: 1) {
                Text(switchButton)
                    .font(.system(size: 12, weight: .semibold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(detail)
                    .font(.system(size: 10))
                    .foregroundColor(NXTheme.textSecondary)
            }
            Spacer()
        }
        .padding(6)
        .background(NXTheme.canvasObsidian.opacity(0.4))
        .cornerRadius(8)
    }
    
    private func bulletPoint(_ text: String) -> some View {
        HStack(alignment: .top, spacing: 6) {
            Text("•")
                .foregroundColor(NXTheme.secondary)
                .font(.system(size: 12, weight: .bold))
            Text(text)
                .font(.system(size: 11))
                .foregroundColor(NXTheme.textSecondary)
        }
    }
}
