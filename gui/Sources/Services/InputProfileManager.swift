import Foundation
import Combine

public final class InputProfileManager: ObservableObject {
    public static let shared = InputProfileManager()

    private enum StorageKeys {
        static let activeProfileId = "NXNative_ActiveControllerProfileId"
    }

    @Published public var profiles: [ControllerProfile] = []
    @Published public var activeProfileId: UUID = UUID() {
        didSet {
            UserDefaults.standard.set(activeProfileId.uuidString, forKey: StorageKeys.activeProfileId)
            exportActiveMappingToDisk()
        }
    }

    private let fileManager = FileManager.default

    private var profilesDirectory: URL {
        let appSup = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSup.appendingPathComponent("nxnative/controller_profiles", isDirectory: true)
        if !fileManager.fileExists(atPath: dir.path) {
            try? fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        return dir
    }

    public var globalMappingFile: URL {
        let appSup = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSup.appendingPathComponent("nxnative", isDirectory: true)
        if !fileManager.fileExists(atPath: dir.path) {
            try? fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        return dir.appendingPathComponent("input_mapping.json")
    }

    private init() {
        loadProfiles()
    }

    public var activeProfile: ControllerProfile {
        if let found = profiles.first(where: { $0.id == activeProfileId }) {
            return found
        }
        return profiles.first ?? ControllerProfile.nintendoDefault
    }

    public func loadProfiles() {
        let dir = profilesDirectory
        var loaded: [ControllerProfile] = []

        if let files = try? fileManager.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil) {
            for file in files where file.pathExtension.lowercased() == "json" {
                if let data = try? Data(contentsOf: file),
                   let prof = try? JSONDecoder().decode(ControllerProfile.self, from: data) {
                    loaded.append(prof)
                }
            }
        }

        if loaded.isEmpty {
            let nintendo = ControllerProfile.nintendoDefault
            let xbox = ControllerProfile.xboxLayout
            let ps = ControllerProfile.playstationLayout
            loaded = [nintendo, xbox, ps]
            for p in loaded {
                saveProfileToDisk(p)
            }
        }

        self.profiles = loaded

        if let savedIdStr = UserDefaults.standard.string(forKey: StorageKeys.activeProfileId),
           let uuid = UUID(uuidString: savedIdStr),
           loaded.contains(where: { $0.id == uuid }) {
            self.activeProfileId = uuid
        } else {
            self.activeProfileId = loaded.first?.id ?? UUID()
        }

        exportActiveMappingToDisk()
    }

    public func updateProfile(_ profile: ControllerProfile) {
        if let idx = profiles.firstIndex(where: { $0.id == profile.id }) {
            profiles[idx] = profile
            saveProfileToDisk(profile)
            if activeProfileId == profile.id {
                exportActiveMappingToDisk()
            }
        }
    }

    public func createProfile(name: String, basedOn source: ControllerProfile? = nil) -> ControllerProfile {
        let base = source ?? activeProfile
        let newProfile = ControllerProfile(
            name: name,
            isDefault: false,
            targetDeviceName: base.targetDeviceName,
            gamepadBindings: base.gamepadBindings,
            keyboardBindings: base.keyboardBindings,
            keyboardMovement: base.keyboardMovement,
            analogDeadzone: base.analogDeadzone
        )
        profiles.append(newProfile)
        saveProfileToDisk(newProfile)
        activeProfileId = newProfile.id
        return newProfile
    }

    public func duplicateProfile(_ profile: ControllerProfile) -> ControllerProfile {
        createProfile(name: "\(profile.name) (Copy)", basedOn: profile)
    }

    public func deleteProfile(_ profile: ControllerProfile) {
        guard !profile.isDefault && profiles.count > 1 else { return }
        profiles.removeAll(where: { $0.id == profile.id })
        let file = profilesDirectory.appendingPathComponent("\(profile.id.uuidString).json")
        try? fileManager.removeItem(at: file)

        if activeProfileId == profile.id {
            activeProfileId = profiles.first?.id ?? UUID()
        }
    }

    public func resetActiveProfileToDefault() {
        let def = ControllerProfile.nintendoDefault
        var current = activeProfile
        current.gamepadBindings = def.gamepadBindings
        current.keyboardBindings = def.keyboardBindings
        current.keyboardMovement = def.keyboardMovement
        current.analogDeadzone = def.analogDeadzone
        updateProfile(current)
    }

    private func saveProfileToDisk(_ profile: ControllerProfile) {
        let file = profilesDirectory.appendingPathComponent("\(profile.id.uuidString).json")
        if let data = try? JSONEncoder().encode(profile) {
            try? data.write(to: file)
        }
    }

    public func exportActiveMappingToDisk(libraryFolder: URL? = nil) {
        let dict = activeProfile.exportDictionary()
        guard let data = try? JSONSerialization.data(withJSONObject: dict, options: [.prettyPrinted]) else { return }

        // 1. Global mapping in Application Support
        try? data.write(to: globalMappingFile)

        // 2. Also write to active game bundles if provided or from saved library folder
        let targetLib = libraryFolder ?? UserDefaults.standard.string(forKey: "NXNative_SavedLibraryFolder").flatMap { URL(fileURLWithPath: $0) }
        if let lib = targetLib, let items = try? fileManager.contentsOfDirectory(at: lib, includingPropertiesForKeys: nil) {
            for item in items where item.pathExtension == "app" {
                let target = item.appendingPathComponent("Contents/Resources/dm/input_mapping.json")
                if fileManager.fileExists(atPath: item.appendingPathComponent("Contents/Resources/dm").path) {
                    try? data.write(to: target)
                }
            }
        }
    }
}
