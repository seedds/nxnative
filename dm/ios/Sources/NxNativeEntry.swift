#if canImport(DmIOSCore)
import UIKit
import DmIOSCore

@main
enum NxNativeEntry {
    static func main() {
        _ = dm_ios_boot_env()
        UIApplicationMain(
            CommandLine.argc,
            CommandLine.unsafeArgv,
            nil,
            NSStringFromClass(DmIOSAppDelegate.self)
        )
    }
}
#endif
