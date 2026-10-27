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
        
        if let sound = cachedSounds[effect] {
            if sound.isPlaying {
                sound.stop()
            }
            sound.play()
        }
    }
}
