import Foundation
import Carbon.HIToolbox

enum KeyNames {
    /// Keys whose label does not depend on the keyboard layout (named in `name(for:)`).
    private static let layoutIndependent: Set<UInt16> = [36, 48, 49, 51, 52, 53, 57, 63, 71, 76, 102, 104, 110, 114, 117, 119, 121, 115, 116]

    /// Printable keys are labelled by what the active layout prints on them. keyCodes are physical
    /// positions, so a fixed US table shows the wrong name on ABNT2 and other layouts
    /// (ABNT2: the key right of L prints "ç" but sits at the US ";" position).
    private static func layoutLabel(for keyCode: UInt16) -> String? {
        guard !layoutIndependent.contains(keyCode), keyCode < 54,
              let source = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue(),
              let ptr = TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) else { return nil }
        let data = Unmanaged<CFData>.fromOpaque(ptr).takeUnretainedValue() as Data
        var deadKeys: UInt32 = 0
        var length = 0
        var chars = [UniChar](repeating: 0, count: 4)
        let status: OSStatus = data.withUnsafeBytes { raw in
            guard let layout = raw.bindMemory(to: UCKeyboardLayout.self).baseAddress else { return -1 }
            return UCKeyTranslate(layout, keyCode, UInt16(kUCKeyActionDisplay), 0, UInt32(LMGetKbdType()),
                                  OptionBits(kUCKeyTranslateNoDeadKeysBit), &deadKeys, chars.count, &length, &chars)
        }
        guard status == noErr, length > 0 else { return nil }
        let label = String(utf16CodeUnits: chars, count: length).uppercased()
        guard let scalar = label.unicodeScalars.first, scalar.value > 32, scalar.value != 127 else { return nil }
        return label
    }

    static func name(for keyCode: UInt16) -> String {
        if let label = layoutLabel(for: keyCode) { return label }
        return tableName(for: keyCode)
    }

    private static func tableName(for keyCode: UInt16) -> String {
        if let mouseName = MouseKeyCode.name(for: keyCode) {
            return mouseName
        }
        switch keyCode {
        case 0: return "A"
        case 1: return "S"
        case 2: return "D"
        case 3: return "F"
        case 4: return "H"
        case 5: return "G"
        case 6: return "Z"
        case 7: return "X"
        case 8: return "C"
        case 9: return "V"
        case 11: return "B"
        case 12: return "Q"
        case 13: return "W"
        case 14: return "E"
        case 15: return "R"
        case 16: return "Y"
        case 17: return "T"
        case 18: return "1"
        case 19: return "2"
        case 20: return "3"
        case 21: return "4"
        case 22: return "6"
        case 23: return "5"
        case 24: return "="
        case 25: return "9"
        case 26: return "7"
        case 27: return "-"
        case 28: return "8"
        case 29: return "0"
        case 30: return "]"
        case 31: return "O"
        case 32: return "U"
        case 33: return "["
        case 34: return "I"
        case 35: return "P"
        case 36: return "Return"
        case 37: return "L"
        case 38: return "J"
        case 39: return "'"
        case 40: return "K"
        case 41: return ";"
        case 42: return "\\"
        case 43: return ","
        case 44: return "/"
        case 45: return "N"
        case 46: return "M"
        case 47: return "."
        case 48: return "Tab"
        case 49: return "Space"
        case 50: return "`"
        case 51: return "Delete"
        case 53: return "Esc"
        case 54: return "R-Cmd"
        case 55: return "Cmd"
        case 56: return "Shift"
        case 57: return "Caps"
        case 58: return "Option"
        case 59: return "Control"
        case 60: return "R-Shift"
        case 61: return "R-Option"
        case 62: return "R-Control"
        case 63: return "Fn"
        case 65: return "Pad ."
        case 67: return "Pad *"
        case 69: return "Pad +"
        case 71: return "Clear"
        case 75: return "Pad /"
        case 76: return "Pad Enter"
        case 78: return "Pad -"
        case 81: return "Pad ="
        case 82: return "Pad 0"
        case 83: return "Pad 1"
        case 84: return "Pad 2"
        case 85: return "Pad 3"
        case 86: return "Pad 4"
        case 87: return "Pad 5"
        case 88: return "Pad 6"
        case 89: return "Pad 7"
        case 91: return "Pad 8"
        case 92: return "Pad 9"
        case 96: return "F5"
        case 97: return "F6"
        case 98: return "F7"
        case 99: return "F3"
        case 100: return "F8"
        case 101: return "F9"
        case 103: return "F11"
        case 109: return "F10"
        case 111: return "F12"
        case 114: return "Help"
        case 115: return "Home"
        case 116: return "PageUp"
        case 117: return "Fwd-Del"
        case 118: return "F4"
        case 119: return "End"
        case 120: return "F2"
        case 121: return "PageDown"
        case 122: return "F1"
        case 123: return "←"
        case 124: return "→"
        case 125: return "↓"
        case 126: return "↑"
        case 10: return "§"
        case 52: return "Enter"
        case 64: return "F17"
        case 79: return "F18"
        case 80: return "F19"
        case 90: return "F20"
        case 93: return "¥"
        case 94: return "_"
        case 95: return "Pad ,"
        case 102: return "Eisu"
        case 104: return "Kana"
        case 105: return "F13"
        case 106: return "F16"
        case 107: return "F14"
        case 110: return "Menu"
        case 113: return "F15"
        default: return "Key #\(keyCode)"
        }
    }
}
