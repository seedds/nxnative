import SwiftUI
import AppKit

/// Captures the next key, modifier or mouse button while `InputProfileManager.listeningTarget` is set.
struct InputCaptureBridge: NSViewRepresentable {
    func makeNSView(context: Context) -> CaptureNSView { CaptureNSView() }
    func updateNSView(_ nsView: CaptureNSView, context: Context) {}

    /// Device-dependent bits of NSEvent.modifierFlags (IOKit NX_DEVICE*KEYMASK). The generic
    /// flags (.shift, .command...) stay set while the opposite-side key is still held, so releasing
    /// one of two Shift keys would be misread as a press of the released one.
    private static let deviceMasks: [UInt16: UInt] = [
        56: 0x2, 60: 0x4,
        59: 0x1, 62: 0x2000,
        58: 0x20, 61: 0x40,
        55: 0x8, 54: 0x10
    ]

    static func isModifierPress(_ event: NSEvent) -> Bool {
        let code = event.keyCode
        if let mask = deviceMasks[code] { return event.modifierFlags.rawValue & mask != 0 }
        if code == 63 { return event.modifierFlags.contains(.function) }
        // Caps Lock only reports its toggle state, so every event is a press.
        return code == 57
    }

    final class CaptureNSView: NSView {
        private var monitor: Any?

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if window != nil && monitor == nil {
                monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged, .leftMouseDown, .rightMouseDown, .otherMouseDown]) { event in
                    let manager = InputProfileManager.shared
                    guard manager.listeningTarget != nil else { return event }

                    switch event.type {
                    case .keyDown:
                        if event.isARepeat { return nil }
                        if event.keyCode == 53 {
                            manager.cancelListening()
                        } else {
                            manager.bindCapturedInput(keyCode: event.keyCode)
                        }
                        return nil

                    case .flagsChanged:
                        if InputCaptureBridge.isModifierPress(event) {
                            manager.bindCapturedInput(keyCode: event.keyCode)
                        }
                        return nil

                    default:
                        // The click that opened the listening state must not be bound itself.
                        guard ProcessInfo.processInfo.systemUptime - manager.listeningStartTime > 0.20 else { return event }
                        let code: UInt16
                        switch event.type {
                        case .leftMouseDown: code = MouseKeyCode.left
                        case .rightMouseDown: code = MouseKeyCode.right
                        default: code = MouseKeyCode.fromButtonNumber(event.buttonNumber)
                        }
                        if code != 0 { manager.bindCapturedInput(keyCode: code) }
                        return nil
                    }
                }
            } else if window == nil, let m = monitor {
                NSEvent.removeMonitor(m)
                monitor = nil
            }
        }

        deinit {
            if let m = monitor { NSEvent.removeMonitor(m) }
        }
    }
}
