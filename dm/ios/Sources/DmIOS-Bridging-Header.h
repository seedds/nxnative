// The Objective-C functions and C the Swift launchers call: CloudSaves.h (the cloud-save folder pick) and save_archive.h
// (Save data). project.yml points SWIFT_OBJC_BRIDGING_HEADER here; save_archive.h is in the host's folder, which the iOS
// project's header search paths include (Gen/dm.xcconfig) and the Mac launcher passes to clang and swiftc.
#import "CloudSaves.h"
#import "save_archive.h"
