// Atmosphère-format cheat engine (dmnt-compatible). Loads the same cheat files a modded Switch uses
// (atmosphere/contents/<title ID>/cheats/<main build ID>.txt) and runs them on the guest in-process.
// Named hle_* so both builds pick it up by wildcard; see hle_cheat.c for the design.
#pragma once

// Start the engine thread (host_preload, after load_modules: the cheat file is keyed by main's build ID).
// Reads DM_CHEATS_DIR (iOS front end sets it to Documents/cheats), DM_CHEATS=off, DM_CHEAT_RATE, DM_CHEAT_LOG.
// A no-op when no directory is configured.
void cheat_start(void);

// Discover + parse the cheat file and toggles.txt under dir (1 = a cheat file was loaded, 0 = none / rejected).
// The engine thread calls this at start and whenever either file's mtime changes; the unit test drives it directly.
int cheat_reload(const char *dir);

// One VM pass: (re)assemble the program of enabled cheats, then execute it once. The engine thread calls this every
// tick (12 Hz by default, like dmnt); the unit test calls it between assertions.
void cheat_execute(void);

// One maintenance step for baked cheat patches (patch_nso DM_CHEAT_FILE): discovers the cheat flags page on first call,
// then keeps each patch's flag byte in step with the cheat table's toggle state and the held buttons. The engine thread
// calls this every tick before cheat_execute; the unit test drives it directly.
void cheat_baked_pump(void);

// The in-game menu (summoned by the hidden hotkey, see hle_cheat.c; the iOS front end shows the panel): a merged,
// toggleable view of baked patches and file cheats. Callable from the front end's main thread; safe against the engine
// tick. cheat_menu_get fills the row's name and its effective state (a keypress-gated patch reads off until held).
int  cheat_menu_count(void);
void cheat_menu_get(int i, char *name, int cap, int *on);
void cheat_menu_toggle(int i);

// Diagnostics: how many memory operations the guard has rejected so far, and how many cheats are enabled.
int cheat_reject_count(void);
int cheat_enabled_count(void);
