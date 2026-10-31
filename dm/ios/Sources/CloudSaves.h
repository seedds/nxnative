// Cloud saves on the iPhone (dm/host/save_sync.h): the iCloud Drive folder the player picked in the
// launcher's Cloud saves section (Launcher.swift), remembered across launches. The Mac app chooses the
// same folder in its settings window. The host brings the save in from there when the game starts and
// sends it there after each save.
//
// iOS does not let an app remember a path outside its sandbox. It lets it keep a bookmark: the document
// picker grants access to the folder, and the bookmark resolves to it again after a relaunch. The
// bookmark is bound to the app's signing identity, so a re-signed build cannot resolve it, and the
// launcher asks the player to pick the folder again.
//
// A folder is only kept when it can hold the save. One inside the app's own storage never reaches iCloud
// Drive: it is the app's Documents (UIFileSharingEnabled puts it in the Files app) or a copy the picker
// made, since a picker in import mode duplicates what is picked into the app's Inbox — which is what a
// launcher's file-picker fix makes every picker do (LiveContainer's Fix File Picker, its
// TweakLoader/DocumentPicker.m forces asCopy). The host also writes the save there after every save, so
// a folder the app cannot write to is refused as well. A folder read back at boot is judged again, so a
// bookmark an older build stored for a copy is not used to sync every save into the app.

#import <Foundation/Foundation.h>

/// Why a folder at `path` may not be the cloud-save folder, or nil when it may: it is inside the app's
/// own storage (`own`: the folders it is made of; CloudSaves.m passes Documents, its Inbox and its temp
/// folder), or it is not a folder. With `must_write` it is refused too when the app cannot write a file
/// into it. The text is a sentence for the player (the launcher's footer, a `[RED]` log line).
/// `must_write` is NO when a stored folder is read back at boot: a write fails for a moment there (no
/// room, an iCloud Drive that is offline), and the host's own `save sync:` lines report it.
NSString *dm_cloud_saves_problem(NSString *path, NSArray<NSString *> *own, BOOL must_write);

/// Store a bookmark for the folder the player just picked. Returns nil when it is remembered, else the
/// sentence for the launcher: the folder is inside the app, cannot be written, or the system refused a
/// bookmark that resolves again.
NSString *dm_store_cloud_saves(NSURL *url);

/// The picked folder, or nil when there is none or it no longer resolves. `stale` (optional) is set
/// when a bookmark exists but cannot be used by this build: it does not resolve under this signature,
/// or it names a folder inside the app (dm_cloud_saves_problem).
NSString *dm_cloud_saves_path(BOOL *stale);

/// Forget it: cloud saves off.
void dm_clear_cloud_saves(void);

/// Puts the picked folder into DM_SAVE_SYNC before the game starts (front_ios.m, at Play), its
/// security scope held for the session. A DM_SAVE_SYNC from an env file or the launch environment
/// wins. Turning cloud saves off in the launcher clears the one this set.
void dm_cloud_saves_env(void);
