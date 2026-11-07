# Godzilla: Destroy All Monsters Melee Remastered (0100A00026D70000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py
# (the repository's and each DM_GAMES_DIR root's) and applies only those of a module's NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches").
#
# No cuts of the engine's own rules: the rewriter already cuts FMallocBinned3's constructor call and sizes Unreal's engine address region for every Unreal game
# (dm/tools/ue_malloc.py, ue_region.py). This game needs no FMalloc layout of its own: its vtable has UE 5.6's 32 slots (see README.md, "Memory").
#
# The three cuts below are this build's own: the shipped main has three functions replaced by a forward through the object's vtable / a stored pointer, and a fresh
# rewrite that runs those functions instead crashes at main+0xbc584 (a null vtable read) inside nninitStartup's DT_INIT, before any frame. Copied byte for byte
# from the shipped image (the same bytes the NSP produces), so a rebuilt main is identical to the shipped one word for word.
CUTS = {'b28a1cc08f675a437de4d399e239bbaa': [
    (0xb7f20, 'ff0301d1fd7b01a9f65702a9', '080040f9081940f900011fd6',
     'the shipped image forwards this function to this->vtable[3] instead of running it: its body crashes at main+0xbc584 (a null vtable read) during nninitStartup DT_INIT'),
    (0xb8530, 'ff4301d1fd7b01a9f71300f9', '080040f9082940f900011fd6',
     'the shipped image forwards this function to this->vtable[5] instead of running it (same DT_INIT crash)'),
    (0x1ffa2c, 'e1031f2a30f6fa973bf6fa97', '211c049021a00391810200f9',
     'the shipped image stores a pointer here instead of calling the two functions (same DT_INIT crash)'),
]}

# No observation gates: the boot hang they were written for is resolved (README.md, "Frame rate" and "The boot hang"). The three sites are recorded in git history
# if it has to be traced again.
OBS = {}

BINDING = [
    {'what': 'three prologue cuts of main (DT_INIT null-vtable crash)', 'now': 'build', 'goal': 'game',
     'purpose': 'runtime', 'why': 'the offsets are this build\'s, but they are the shipped image\'s own bytes: any build of this title needs them, and a rebuild from the same update reproduces them',
     'kept': 'permanent for 1.0.2; a new update needs its own build ID and its own check'},
]
