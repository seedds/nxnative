import SwiftUI

public enum LauncherHighlightTheme: String, CaseIterable, Identifiable {
    case electricPurple = "Electric Purple"
    case nintendoRed = "Switch Neon Red"
    case neonBlue = "Switch Neon Blue"
    case emeraldGreen = "Emerald Green"
    case sunsetOrange = "Sunset Orange"
    case cyberRose = "Cyber Rose"
    case hyruleGold = "Hyrule Gold"
    case deepCobalt = "Deep Cobalt"
    
    public var id: String { rawValue }
    
    public var subtitle: String {
        switch self {
        case .electricPurple: return "Default Signature Violet"
        case .nintendoRed:    return "Classic Joy-Con Red"
        case .neonBlue:       return "Classic Joy-Con Blue"
        case .emeraldGreen:   return "Vibrant Cyber Emerald"
        case .sunsetOrange:   return "Warm Solar Accent"
        case .cyberRose:      return "Neon Magenta & Pink"
        case .hyruleGold:     return "Royal Triforce Amber"
        case .deepCobalt:     return "Clean macOS Sapphire"
        }
    }
    
    public var hexLabel: String {
        switch self {
        case .electricPurple: return "#6F31FF"
        case .nintendoRed:    return "#E60012"
        case .neonBlue:       return "#0084FF"
        case .emeraldGreen:   return "#10B981"
        case .sunsetOrange:   return "#F97316"
        case .cyberRose:      return "#EC4899"
        case .hyruleGold:     return "#D97706"
        case .deepCobalt:     return "#3B82F6"
        }
    }
    
    public var primaryColor: Color {
        switch self {
        case .electricPurple:
            return Color(red: 0x6F / 255.0, green: 0x31 / 255.0, blue: 0xFF / 255.0)
        case .nintendoRed:
            return Color(red: 0xE6 / 255.0, green: 0x00 / 255.0, blue: 0x12 / 255.0)
        case .neonBlue:
            return Color(red: 0x00 / 255.0, green: 0x84 / 255.0, blue: 0xFF / 255.0)
        case .emeraldGreen:
            return Color(red: 0x10 / 255.0, green: 0xB9 / 255.0, blue: 0x81 / 255.0)
        case .sunsetOrange:
            return Color(red: 0xF9 / 255.0, green: 0x73 / 255.0, blue: 0x16 / 255.0)
        case .cyberRose:
            return Color(red: 0xEC / 255.0, green: 0x48 / 255.0, blue: 0x99 / 255.0)
        case .hyruleGold:
            return Color(red: 0xD9 / 255.0, green: 0x77 / 255.0, blue: 0x06 / 255.0)
        case .deepCobalt:
            return Color(red: 0x3B / 255.0, green: 0x82 / 255.0, blue: 0xF6 / 255.0)
        }
    }
    
    public var secondaryColor: Color {
        switch self {
        case .electricPurple:
            return Color(red: 0x8B / 255.0, green: 0x5C / 255.0, blue: 0xF6 / 255.0)
        case .nintendoRed:
            return Color(red: 0xFF / 255.0, green: 0x4B / 255.0, blue: 0x55 / 255.0)
        case .neonBlue:
            return Color(red: 0x00 / 255.0, green: 0xC2 / 255.0, blue: 0xFF / 255.0)
        case .emeraldGreen:
            return Color(red: 0x34 / 255.0, green: 0xD3 / 255.0, blue: 0x99 / 255.0)
        case .sunsetOrange:
            return Color(red: 0xFB / 255.0, green: 0x92 / 255.0, blue: 0x3C / 255.0)
        case .cyberRose:
            return Color(red: 0xF4 / 255.0, green: 0x72 / 255.0, blue: 0xB6 / 255.0)
        case .hyruleGold:
            return Color(red: 0xF5 / 255.0, green: 0x9E / 255.0, blue: 0x0B / 255.0)
        case .deepCobalt:
            return Color(red: 0x60 / 255.0, green: 0xA5 / 255.0, blue: 0xFA / 255.0)
        }
    }
}
