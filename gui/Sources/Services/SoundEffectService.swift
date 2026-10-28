import Foundation
import AppKit

public enum SoundEffectType: CaseIterable {
    case click
    case tabSwitch
    case focus
    case launch
    case error
}

public final class SoundEffectService: ObservableObject {
    public static let shared = SoundEffectService()
    
    private let soundKey = "NXNative_SoundEnabled"
    private var cachedSounds: [SoundEffectType: NSSound] = [:]
    private var lastPlayTimes: [SoundEffectType: Date] = [:]
    private var lastGlobalPlayTime: Date = Date.distantPast
    
    @Published public var isSoundEnabled: Bool {
        didSet {
            UserDefaults.standard.set(isSoundEnabled, forKey: soundKey)
        }
    }
    
    private init() {
        if UserDefaults.standard.object(forKey: soundKey) == nil {
            self.isSoundEnabled = true
        } else {
            self.isSoundEnabled = UserDefaults.standard.bool(forKey: soundKey)
        }
        
        let mapping: [(SoundEffectType, String)] = [
            (.click, "Tink"),
            (.tabSwitch, "Pop"),
            (.focus, "Purr"),
            (.launch, "Hero"),
            (.error, "Basso")
        ]
        for (effect, name) in mapping {
            if let sound = NSSound(named: name) {
                cachedSounds[effect] = sound
            }
        }
    }
    
    public func play(_ effect: SoundEffectType) {
        guard isSoundEnabled else { return }
        
        let now = Date()
        // Prevent duplicate trigger bursts of the exact same sound
        if let last = lastPlayTimes[effect], now.timeIntervalSince(last) < 0.12 {
            return
        }
        
        // Prevent rapid overlapping distinct sounds on a single mouse click (e.g. click + focus)
        if effect != .launch && effect != .error {
            if now.timeIntervalSince(lastGlobalPlayTime) < 0.08 {
                return
            }
        }
        
        lastPlayTimes[effect] = now
        lastGlobalPlayTime = now
        
        if let sound = cachedSounds[effect] {
            if sound.isPlaying {
                sound.stop()
            }
            sound.play()
        }
    }
}
