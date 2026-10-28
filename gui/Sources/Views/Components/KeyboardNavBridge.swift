import SwiftUI
import AppKit

// MARK: - Global & Local Keyboard Bridge View (Search/TextField Safe)
public struct KeyboardNavBridge: NSViewRepresentable {
    public let onKey: (String) -> Void
    
    public init(onKey: @escaping (String) -> Void) {
        self.onKey = onKey
    }
    
    public func makeNSView(context: Context) -> KeyboardListenerNSView {
        let view = KeyboardListenerNSView()
        view.onKey = onKey
        return view
    }
    
    public func updateNSView(_ nsView: KeyboardListenerNSView, context: Context) {
        nsView.onKey = onKey
    }
    
    public class KeyboardListenerNSView: NSView {
        var onKey: ((String) -> Void)?
        private var monitor: Any?
        
        public override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if window != nil && monitor == nil {
                monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
                    guard let self = self else { return event }
                    // Prevent search field or text editor hijacking
                    if let fr = self.window?.firstResponder, fr is NSTextView || fr is NSTextField {
                        // Allow Escape to dismiss focus and return control to the library
                        if event.keyCode == 53 {
                            self.window?.makeFirstResponder(nil)
                            return nil
                        }
                        // Allow Command + 1..7 tab switching while inside text fields
                        if event.modifierFlags.contains(.command),
                           let chars = event.charactersIgnoringModifiers,
                           let first = chars.first,
                           ("1"..."7").contains(String(first)) {
                            self.window?.makeFirstResponder(nil)
                            self.onKey?(String(first))
                            return nil
                        }
                        return event
                    }
                    
                    // Special keys by keyCode
                    switch event.keyCode {
                    case 48: // Tab key
                        let isShift = event.modifierFlags.contains(.shift)
                        self.onKey?(isShift ? "shifttab" : "tab")
                        return nil
                    case 33: // [ key
                        self.onKey?("[")
                        return nil
                    case 30: // ] key
                        self.onKey?("]")
                        return nil
                    case 123: // Left Arrow
                        self.onKey?("left")
                        return nil
                    case 124: // Right Arrow
                        self.onKey?("right")
                        return nil
                    case 126: // Up Arrow
                        self.onKey?("up")
                        return nil
                    case 125: // Down Arrow
                        self.onKey?("down")
                        return nil
                    case 36, 76: // Return / Enter
                        self.onKey?("return")
                        return nil
                    case 53: // Escape
                        self.onKey?("escape")
                        return nil
                    case 49: // Spacebar
                        self.onKey?("space")
                        return nil
                    default:
                        break
                    }
                    
                    // Layout-independent character keys (numbers, letters, symbols)
                    if let chars = event.charactersIgnoringModifiers, let first = chars.first {
                        let str = String(first).lowercased()
                        if ("1"..."7").contains(str) || ["o", ",", "r", "c", "k", "i", "u", "q", "e"].contains(str) {
                            if str == "q" {
                                self.onKey?("[")
                                return nil
                            }
                            if str == "e" {
                                self.onKey?("]")
                                return nil
                            }
                            self.onKey?(str)
                            return nil
                        }
                    }
                    
                    return event
                }
            } else if window == nil, let m = monitor {
                NSEvent.removeMonitor(m)
                monitor = nil
            }
        }
        
        deinit {
            if let m = monitor {
                NSEvent.removeMonitor(m)
            }
        }
    }
}
