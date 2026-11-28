// The C and Objective-C functions Launcher.swift, GameMenu.swift, LauncherSettings.swift and DmHostingController.swift call (CloudSaves.h, DmGameMenu.h, DmRotation.h, save_archive.h for Save data,
// save_sync.h's report before Play, the screen's rate). project.yml points SWIFT_OBJC_BRIDGING_HEADER here; save_archive.h is in the host's folder, which the iOS project's header search paths
// include (Gen/dm.xcconfig) and the Mac launcher passes to clang and swiftc.
#import "CloudSaves.h"
#import "DmGameMenu.h"
#import "DmRotation.h"
#import "save_archive.h"
#include "save_sync.h"
int front_screen_max_fps(void);   // front.h, front_ios.m: the Frame rate row offers 40 and 120 only on a 120 Hz screen (LauncherSettings.swift offeredOptions)
