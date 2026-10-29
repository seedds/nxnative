import Foundation
import AppKit

public enum AppIconAppearanceMode: String, CaseIterable, Identifiable, Hashable {
    case light = "Light"
    case dark = "Dark"
    case mono = "Monochrome"
    case auto = "Auto (System)"
    
    public var id: String { rawValue }
}

public enum AppIconCache {
    private static var cache: [String: NSImage] = [:]
    private static let lock = NSLock()

    public static func image(named name: String) -> NSImage? {
        lock.lock()
        if let existing = cache[name] {
            lock.unlock()
            return existing
        }
        lock.unlock()

        let url = Bundle.main.url(forResource: name, withExtension: "png")
            ?? Bundle.main.url(forResource: name, withExtension: "icns")
            ?? Bundle.main.url(forResource: "AppIcon", withExtension: "png")
            ?? Bundle.main.url(forResource: "AppIcon", withExtension: "icns")

        guard let targetURL = url, let img = NSImage(contentsOf: targetURL) else {
            return nil
        }

        lock.lock()
        cache[name] = img
        lock.unlock()
        return img
    }
}
