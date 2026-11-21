# Cytus α (`010063100B2C2000`)

Update 1.0.6 (v393216) over the base game, SDK 6.4.2. Unity 2018.2.18f1 IL2CPP with FMOD, on NVN. Modules `main`, `subsdk0` (the movie module) and `sdk`; `sdk` and `subsdk0` are AI: THE SOMNIUM FILES' builds ([010089B00D09C000](../010089B00D09C000/README.md)), so their cuts and host rows apply as they are. Plays on the Mac and on the iPhone; 60 fps (present interval 1). A rhythm game played on the touch screen. The RomFS is 2.2 GB (1,276 files, nearly all UnityFS bundles); the IPA with the game files bundled is 1.8 GB, without them (`--romfs separate`) the game files go to `Documents/RomFS.img`.

Brought up on the Mac and on an iPhone Duo (A20 Pro, iOS 27.1): boot, the two startup movies, the title, song select, and a whole song played with touches (SIN - THE LAST SCENE -, Easy 5, a full combo, the result screen). No `[RED]` line on either. The provisional profile of `scripts/build_ipa_from_nsp.py` booted as it was; the profile keeps its guessed keys and adds the mode keys below.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `op_mode handheld` | [`game.tsv`](game.tsv) | the game reads the touch screen only in handheld mode: docked, a click on the Mac and a touch on the iPhone reach `GetTouchScreenState` but the game ignores them, as on a Switch in its dock |
| `pad_layout touch` | `game.tsv` | the Tap layout: no drawn control, every touch goes to the game |
| `perf_mode boost` | `game.tsv` | handheld Normal draws 1280x720 (`WindowCrop 0,0 1280x720`); Boost in handheld, a combination a Switch 1 never reports, gives the docked 1920x1080 draw size with the touch screen still read |
| `touch_panel 1920x1080` | `game.tsv` | under that Boost the game takes touches against its 1920x1080 draw size, not the 1280x720 panel: with the hardware panel's coordinates every touch landed short toward the top left. See [Touch](#touch) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | as AI: THE SOMNIUM FILES: no system resource, so no virtual address memory; Unity sets a 3.14 GB heap in the 3.25 GB heap region. The default 64 MB of used memory leaves room for it (Somnium needs 104 MB, Cytus' `main` is smaller) |

No `patches.py`, and `dm/tools/sdk_patches.py` has no cut for its `sdk` build (`184751379007f9c50fc96ba4d26a5dca`).

## Touch

- `hle_touch.c` hands the game touch points on the 1280x720 panel, as hardware does. The profile key `touch_panel` (default `1280x720`) sets that range; `DM_TOUCH_PANEL=<w>x<h>` overrides it for a run, and the log says `[touch] panel <w>x<h>` at the first read.
- Handheld Normal (no `perf_mode boost`, 1280x720 drawn) took the hardware panel's coordinates correctly; only the Boost combination needs `touch_panel`.
- Touch judgement in play matched the music on the iPhone: a full combo on an Easy chart. The iPhone log gives game enqueue to screen p50 48 ms (the present queue's three frames), which the game's own offset setting covers.

## Memory and frame rate

- iPhone Duo, 3.5 minutes from boot through a song: footprint 0.25 → 0.83 GB as menus and the song loaded, flat at about 0.75 GB through the song; headroom to the system memory limit 2.5 GB or more. Frame time p50 16.67 ms, no dropped frame through the song, thermal state 0.
- Mac: 1.26 → 1.48 GB resident over the first 30 s, 60 fps.

## Movies

- `subsdk0` plays the two startup movies (H.264 Main 1920x1080 and AAC, 60 pictures a second) through VideoToolbox; both ran to their end and the game went on.

## Files and shaders

- 488 shaders from the UnityFS bundles and `main` (`shader_split.py` through `unityfs.py`; Unity 2018.2 read with the 2017.4 / 2018.3 layout), all built for Mac and iOS.
- `romfs_marker` is `Data/StreamingAssets/Ingame/Atlas/MillionMaster.unity3d`, the largest RomFS file.

## Open

- The iPhone app showed the 2D gamepad at first start once instead of the Tap layout the profile names; not reproduced (the choice is kept per app, so it took a reinstall to try again).
- Docked play with buttons was not tried.
