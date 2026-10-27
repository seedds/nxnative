import SwiftUI

// MARK: - ControllerPromptBar Badges, Buttons and Color Helpers
extension ControllerPromptBar {
    
    // MARK: - Controller True Colors
    var brandAColor: Color {
        switch controllerService.activeBrand {
        case .xbox, .generic:
            return Color(red: 0.06, green: 0.49, blue: 0.06) // Authentic Xbox Green #107C10
        case .playstation:
            return Color(red: 0.18, green: 0.43, blue: 0.71) // PlayStation Blue
        case .nintendoSwitch:
            return Color(red: 0.90, green: 0.0, blue: 0.07)  // Nintendo Red #E60012
        }
    }
    
    var brandBColor: Color {
        switch controllerService.activeBrand {
        case .xbox, .generic:
            return Color(red: 0.91, green: 0.07, blue: 0.14) // Authentic Xbox Red #E81123
        case .playstation:
            return Color(red: 0.87, green: 0.0, blue: 0.14)  // PlayStation Red
        case .nintendoSwitch:
            return Color(red: 0.0, green: 0.52, blue: 0.87)  // Nintendo Blue
        }
    }
    
    var brandXColor: Color {
        switch controllerService.activeBrand {
        case .xbox, .generic:
            return Color(red: 0.0, green: 0.47, blue: 0.84)  // Authentic Xbox Blue #0078D7
        case .playstation:
            return Color(red: 0.80, green: 0.36, blue: 0.58) // PlayStation Pink/Magenta
        case .nintendoSwitch:
            return Color(red: 0.0, green: 0.52, blue: 0.87)  // Nintendo Blue
        }
    }
    
    var brandYColor: Color {
        switch controllerService.activeBrand {
        case .xbox, .generic:
            return Color(red: 1.0, green: 0.73, blue: 0.0)   // Authentic Xbox Yellow #FFB900
        case .playstation:
            return Color(red: 0.0, green: 0.78, blue: 0.59) // PlayStation Green
        case .nintendoSwitch:
            return Color(red: 0.90, green: 0.0, blue: 0.07)  // Nintendo Red
        }
    }
    
    var brandOptionsColor: Color {
        Color(white: 0.22) // Graphite slate pill
    }
    
    // MARK: - Shoulder Bumper View (Properly Sized, Centered, Never Clipped)
    var shoulderTabsPromptView: some View {
        HStack(spacing: 5) {
            HStack(spacing: 3) {
                bumperCap(text: controllerService.activeBrand.leftShoulderName)
                bumperCap(text: controllerService.activeBrand.rightShoulderName)
            }
            Text("Tabs")
                .font(.system(size: 11, weight: .medium, design: .rounded))
                .foregroundColor(.white)
        }
    }
    
    func bumperCap(text: String) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .bold, design: .rounded))
            .foregroundColor(.white)
            .padding(.horizontal, 6)
            .padding(.vertical, 2.5)
            .background(
                Capsule()
                    .fill(Color(white: 0.22))
            )
            .overlay(
                Capsule()
                    .strokeBorder(Color.white.opacity(0.25), lineWidth: 0.8)
            )
    }
    
    // MARK: - Controller Pills (Centered Glyph inside Background Circle)
    func controllerPill(glyph: String, action: String, color: Color) -> some View {
        HStack(spacing: 5) {
            Text(glyph)
                .font(.system(size: 10, weight: .black, design: .rounded))
                .foregroundColor(.white)
                .frame(width: 18, height: 18, alignment: .center)
                .background(Circle().fill(color))
                .shadow(color: color.opacity(0.45), radius: 3)
            
            Text(action)
                .font(.system(size: 11, weight: .medium, design: .rounded))
                .foregroundColor(.white)
        }
    }
    
    func playPromptPill(aGlyph: String, xGlyph: String) -> some View {
        HStack(spacing: 5) {
            HStack(spacing: 2) {
                Text(aGlyph)
                    .font(.system(size: 10, weight: .black, design: .rounded))
                    .foregroundColor(.white)
                    .frame(width: 18, height: 18, alignment: .center)
                    .background(Circle().fill(brandAColor))
                
                Text("/")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.white)
                
                Text(xGlyph)
                    .font(.system(size: 10, weight: .black, design: .rounded))
                    .foregroundColor(.white)
                    .frame(width: 18, height: 18, alignment: .center)
                    .background(Circle().fill(brandXColor))
            }
            
            Text("Play")
                .font(.system(size: 11, weight: .semibold, design: .rounded))
                .foregroundColor(.white)
        }
    }
    
    func optionsPill(glyph: String, action: String) -> some View {
        HStack(spacing: 5) {
            Text(glyph)
                .font(.system(size: 9, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 5)
                .padding(.vertical, 2.5)
                .background(Capsule().fill(brandOptionsColor))
                .overlay(
                    Capsule().strokeBorder(Color.white.opacity(0.2), lineWidth: 0.8)
                )
            
            Text(action)
                .font(.system(size: 11, weight: .medium, design: .rounded))
                .foregroundColor(.white)
        }
    }
    
    func dpadPill(glyph: String, action: String) -> some View {
        HStack(spacing: 5) {
            Text(glyph)
                .font(.system(size: 9, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 5)
                .padding(.vertical, 2)
                .background(Capsule().fill(Color(white: 0.2)))
                .overlay(
                    Capsule().strokeBorder(Color.white.opacity(0.18), lineWidth: 0.8)
                )
            
            Text(action)
                .font(.system(size: 11, weight: .medium, design: .rounded))
                .foregroundColor(.white)
        }
    }
    
    func keyboardKeyBadge(glyph: String, action: String) -> some View {
        HStack(spacing: 5) {
            Text(glyph)
                .font(.system(size: 10, weight: .semibold, design: .monospaced))
                .foregroundColor(.white)
                .padding(.horizontal, 6)
                .padding(.vertical, 2.5)
                .background(
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .fill(Color(white: 0.18))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .strokeBorder(Color.white.opacity(0.2), lineWidth: 0.8)
                )
                .shadow(color: Color.black.opacity(0.3), radius: 1, x: 0, y: 1)
            
            Text(action)
                .font(.system(size: 11, weight: .medium, design: .rounded))
                .foregroundColor(.white)
        }
    }
}
