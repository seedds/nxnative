// Rotation (DM_ROTATION, a launcher and in-game menu setting: auto, landscape, portrait; DmLayout.h dm_rotation_mask): front_ios.m keeps it, the root view controller, the launcher's
// hosting controller (DmHostingController.swift) and the app delegate (main.m) ask it which orientations they allow.
#pragma once
#import <UIKit/UIKit.h>

#ifdef __cplusplus
extern "C" {
#endif
UIInterfaceOrientationMask dm_rotation_vc_mask(void);   // a full-screen view controller's: every orientation under auto (the app's list decides)
UIInterfaceOrientationMask front_ios_app_orientations(void);   // the app's (application:supportedInterfaceOrientationsForWindow:): under auto its Info.plist list for this device
void dm_rotation_set(const char *_Nullable value);   // main thread: DM_ROTATION = value (NULL keeps the environment's), and the windows turn to an orientation it allows
#ifdef __cplusplus
}
#endif
