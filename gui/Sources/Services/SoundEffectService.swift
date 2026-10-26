import Foundation
import AppKit

public enum SoundEffectType {
    case click
    case tabSwitch
    case focus
    case launch
    case error
}

public final class SoundEffectService: ObservableObject {
    public static let shared = SoundEffectService()
    
    private let soundKey = "NXNative_SoundEnabled"
    
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
    }
    
    public func play(_ effect: SoundEffectType) {
        guard isSoundEnabled else { return }
        
        DispatchQueue.main.async {
            switch effect {
            case .click:
                NSSound(named: "Tink")?.play()
            case .tabSwitch:
                NSSound(named: "Pop")?.play()
            case .focus:
                NSSound(named: "Purr")?.play()
            case .launch:
                NSSound(named: "Hero")?.play()
            case .error:
                NSSound(named: "Basso")?.play()
            }
        }
    }
}
