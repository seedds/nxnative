# Cytus α (`010063100B2C2000`)

Update 1.0.6 (v393216) over the base game, SDK 6.4.2. Unity 2018.2.18f1 IL2CPP with FMOD, on NVN. Modules `main`, `subsdk0` (the movie module) and `sdk`; `sdk` and `subsdk0` are AI: THE SOMNIUM FILES' builds ([010089B00D09C000](../010089B00D09C000/README.md)), so their cuts and host rows apply as they are. Plays on the Mac and on the iPhone; 60 fps (present interval 1). A rhythm game played on the touch screen. The RomFS is 2.2 GB (1,276 files, nearly all UnityFS bundles); the IPA with the game files bundled is 1.8 GB, without them (`--romfs separate`) the game files go to `Documents/RomFS.img`.

Brought up on the Mac and on an iPhone Duo (A20 Pro, iOS 27.1): boot, the two startup movies, the title, song select, and a whole song played with touches (SIN - THE LAST SCENE -, Easy 5, a full combo, the result screen). No `[RED]` line on either. The provisional profile of `scripts/build_ipa_from_nsp.py` booted as it was; the profile keeps its guessed keys and adds the mode keys below.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `op_mode handheld` | [`game.tsv`](game.tsv) | the game reads the touch screen only in handheld mode: docked, a click on the Mac and a touch on the iPhone reach `GetTouchScreenState` but the game ignores them, as on a Switch in its dock |
| `pad_layout touch` | `game.tsv` | the Tap layout: no drawn control, every touch goes to the game |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | as AI: THE SOMNIUM FILES: no system resource, so no virtual address memory; Unity sets a 3.14 GB heap in the 3.25 GB heap region. The default 64 MB of used memory leaves room for it (Somnium needs 104 MB, Cytus' `main` is smaller) |

No `patches.py`, and `dm/tools/sdk_patches.py` has no cut for its `sdk` build (`184751379007f9c50fc96ba4d26a5dca`).

## Touch

- `hle_touch.c` hands the game touch points on the hardware 1280x720 panel. Handheld Normal (`op_mode handheld`, no `perf_mode boost`, drawing 1280x720 with `WindowCrop 0,0 1280x720`) takes those coordinates directly with no host workaround.
- **Why not `perf_mode boost`:** Unity's resolution selector (`main+0x1007320`, case 2 at `0x1007390`) switches `Screen` to 1920x1080 from `GetPerformanceMode() == Boost` alone (`main` does not import `GetDefaultDisplayResolution`). Neither touch path scales panel coordinates from 1280x720 to `Screen`: the game's custom native plugin (`main+0x1c2aa60` → `0x1c2b0bc`) stores raw `GetTouchScreenStates<16>` integers and its C# `get_position` (`main+0xc4bb90`) computes `(raw.x, Screen.get_height() - raw.y)` unscaled, so under Boost every touch lands in the top-left 1280x720 of the 1920x1080 screen; meanwhile Unity 2018.2's own `SwitchInput` (`main+0x1000270`) hardcodes `(touch.x, 720 - touch.y)` (`mov w23, #0x2d0`), so scaling the host panel range to 1920x1080 would also produce negative Y in `Input.GetTouch`. Staying in handheld Normal keeps both touch paths consistent with Switch hardware.
- Touch judgement in play matched the music on the iPhone: a full combo on an Easy chart (measured in the PR author's 1080p Boost test; handheld Normal 1280x720 was checked for touch alignment and keeps the same 60 fps pacing). The iPhone log gives game enqueue to screen p50 48 ms (the present queue's three frames), which the game's own offset setting covers.

## Memory and frame rate

- iPhone Duo, 3.5 minutes from boot through a song: footprint 0.25 → 0.83 GB as menus and the song loaded, flat at about 0.75 GB through the song; headroom to the system memory limit 2.5 GB or more. Frame time p50 16.67 ms, no dropped frame through the song, thermal state 0.
- Mac: 1.26 → 1.48 GB resident over the first 30 s, 60 fps.

## Movies

- `subsdk0` plays the two startup movies (H.264 Main 1920x1080 and AAC, 60 pictures a second) through VideoToolbox; both ran to their end and the game went on.

## Files and shaders

- 488 shaders from the UnityFS bundles and `main` (`shader_split.py` through `unityfs.py`; Unity 2018.2 read with the 2017.4 / 2018.3 layout), all built for Mac and iOS.
- `romfs_marker` is `Data/StreamingAssets/Ingame/Atlas/MillionMaster.unity3d`, the largest RomFS file.

## Open

- The full-song iPhone Duo playtest (memory/frame-rate log above) was recorded under the experimental 1080p Boost mode; a full-song iPhone retest in the committed handheld Normal 1280x720 mode is still open (handheld Normal touch alignment was checked by the PR author).
- The iPhone app showed the 2D gamepad at first start once instead of the Tap layout the profile names; not reproduced (the choice is kept per app, so it took a reinstall to try again).
- Docked play with buttons was not tried.
