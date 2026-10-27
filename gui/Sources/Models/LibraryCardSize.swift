import Foundation
import CoreGraphics

public enum LibraryCardSize: String, CaseIterable, Identifiable {
    case compact = "Compact"
    case regular = "Regular"
    case large = "Large"
    
    public var id: String { rawValue }
    
    // Official Nintendo Switch retail box ratio (1 : 1.60)
    public static let portraitRatio: CGFloat = 1.60
    
    // Base widths
    public static let baseUnfocusedWidth: CGFloat = 200
    public static let baseFocusedWidth: CGFloat = 265
    
    public static func unfocusedWidth(scale: Double) -> CGFloat {
        baseUnfocusedWidth * CGFloat(scale)
    }
    
    public static func focusedWidth(scale: Double) -> CGFloat {
        baseFocusedWidth * CGFloat(scale)
    }
    
    public static func unfocusedHeight(scale: Double) -> CGFloat {
        unfocusedWidth(scale: scale) * portraitRatio
    }
    
    public static func focusedHeight(scale: Double) -> CGFloat {
        focusedWidth(scale: scale) * portraitRatio
    }
    
    public static func carouselHeight(scale: Double) -> CGFloat {
        focusedHeight(scale: scale) + 70
    }
    
    // Backwards-compatible properties
    public var unfocusedWidth: CGFloat {
        switch self {
        case .compact: return 160
        case .regular: return 200
        case .large: return 255
        }
    }
    
    public var focusedWidth: CGFloat {
        switch self {
        case .compact: return 215
        case .regular: return 265
        case .large: return 340
        }
    }
    
    public var unfocusedHeight: CGFloat { unfocusedWidth * Self.portraitRatio }
    public var focusedHeight: CGFloat { focusedWidth * Self.portraitRatio }
    public var carouselHeight: CGFloat { focusedHeight + 70 }
}
