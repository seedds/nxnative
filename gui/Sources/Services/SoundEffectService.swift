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
    private let lock = NSLock()
    
    // Minimum interval between identical sound triggers (prevents double-tap / gesture bounce)
    private let debounceInterval: TimeInterval = 0.22
    
    // Minimum interval between distinct UI sounds (prevents click + focus or card + button collision)
    private let globalThrottleInterval: TimeInterval = 0.18
    
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
        
        let coreSystemPath = "/System/Library/Components/CoreAudio.component/Contents/SharedSupport/SystemSounds/system"
        let mediaKeysPath = "\(coreSystemPath)/Media Keys.aif"
        let tabSoundPath = "\(coreSystemPath)/acknowledgment_sent.caf"
        
        // Single-impulse, non-overlapping sounds
        cachedSounds[.click] = loadSound(fromPath: mediaKeysPath, fallbackNamed: "Tink")
        cachedSounds[.tabSwitch] = loadSound(fromPath: tabSoundPath, fallbackNamed: "Tink")
        cachedSounds[.focus] = loadSound(fromPath: mediaKeysPath, fallbackNamed: "Tink")
        cachedSounds[.launch] = NSSound(named: "Hero")
        cachedSounds[.error] = NSSound(named: "Basso")
    }
    
    private func loadSound(fromPath path: String, fallbackNamed name: String) -> NSSound? {
        if FileManager.default.fileExists(atPath: path),
           let sound = NSSound(contentsOfFile: path, byReference: true) {
            return sound
        }
        return NSSound(named: name)
    }
    
    public func play(_ effect: SoundEffectType) {
        guard isSoundEnabled else { return }
        
        lock.lock()
        let now = Date()
        
        // 1. Prevent duplicate trigger bursts of the exact same sound
        if let last = lastPlayTimes[effect], now.timeIntervalSince(last) < debounceInterval {
            lock.unlock()
            return
        }
        
        // 2. Prevent rapid overlapping distinct sounds on a single mouse click (e.g. click + focus or click + launch)
        if effect != .error {
            if now.timeIntervalSince(lastGlobalPlayTime) < globalThrottleInterval {
                lock.unlock()
                return
            }
        }
        
        lastPlayTimes[effect] = now
        lastGlobalPlayTime = now
        let sound = cachedSounds[effect]
        lock.unlock()
        
        guard let sound = sound else { return }
        
        DispatchQueue.main.async {
            // Use independent copy to avoid stopping or clipping audio buffer in AppKit CoreAudio
            if let player = sound.copy() as? NSSound {
                player.play()
            } else {
                if sound.isPlaying {
                    sound.stop()
                }
                sound.play()
            }
        }
    }
}
