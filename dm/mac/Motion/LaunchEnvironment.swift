import Foundation
// Explicit launch overrides always win. Configuration is loaded at launch, never baked into a bundle.
@_cdecl("dm_mac_motion_apply")
public func dmMacMotionApply() {
    do {
        for (key,value) in try DSUConfiguration.load().environment() {
            if getenv(key)==nil { setenv(key,value,0) }
        }
    } catch { fputs("[RED] launcher: invalid shared DSU configuration, phone motion disabled\n",stderr) }
}
