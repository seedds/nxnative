# Ghost Trick: Phantom Detective (`010029B018432000`)

Base game 1.0.0 (v0), SDK 13.3.0, with the Bonus Content add-on (add-on index 1: songs and backgrounds, unlocked at the first boot). Capcom's RE Engine on NVN, Wwise audio, Havok. Modules `main`, `subsdk0` (the SDK's movie library, NintendoSDK_movie 13.3.0) and `sdk`. Plays on the iPhone (IPA 3.0 GB).

On the Mac, from a fresh save: the opening movies, the language screen, the first-boot notices, save creation, the Bonus Content notices, Chapter 1's opening in the junkyard (the Ghost World, moving between cores, the first trick on the crossing gate, the portraits and dialogue). A second boot loads that save and reaches the title menu. 60 fps (frame interval p99 about 17–19 ms), about 2.3 GB resident and flat after the first scenes, CPU about a quarter of a core (0.3–0.6 W); no red line on that route.

## What it needs

The provisional profile's guesses held; the two texture keys were added after the first boot.

| Setting | Where | Why |
|---|---|---|
| `tex_flush_upload on`, `tex_dirty_upload on` | [`game.tsv`](game.tsv) | RE Engine writes its UI textures (ASTC) into CPU-visible pools in place, as Monster Hunter Rise ([0100B04011742000](../0100B04011742000/README.md)) does; without the keys the language screen's background and buttons were magenta (all-zero ASTC blocks), and with `tex_flush_upload` alone they still were ([CPU writes with no flush, caught by page protection](../../../docs/reference/nvn-objects.md#cpu-writes-with-no-flush-caught-by-page-protection-tex_dirty_upload)) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so there is no virtual address memory: the game sets a heap of the pool − used memory |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | 256 MB alias, 256 MB stacks, 3.25 GB heap region, as Monster Hunter Rise |
| `mem_used 0x87bc000` (135 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory ([Heap sized from the application pool less used memory](../../../docs/reference/memory.md#heap-sized-from-the-application-pool-less-used-memory)) |

No `fps_target`: the game runs at 60 on the Switch. No `pad_layout`: the virtual gamepad starts in the default 2D layout.

## Files and shaders

- One RE Engine PAK, `re_chunk_000.pak` (2.66 GB), and `natives/NSW/runtime.args`. `romfs_marker` is `re_chunk_000.pak`.
- **Current:** 7,744 shaders from the PAK's SDF definitions and `main` (3,731 pixel, 1,559 vertex, 2,401 compute, 20 geometry, 15 hull, 18 domain) and 8,530 pairs ([SDF definitions in RE Engine PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks)). 7,739 built on chain `dec5ee141a9f`; the other 5 are compute shaders that do atomics on a 3D image (`16a78d0fd09ccc18`, `33559ae5667779fe`, `7c65836255e05a87`, `a838b76ccc822be7`, `ecef686222a38548`), which SPIRV-Cross's MSL 2.3 emulation indexed with an `int3` (`array subscript is not an integer`), as Octopath Traveler 0's 16. With native image atomics all 5 build (a build of those keys on chain `0ce4b5e8aefb`, 2026-10-27; `33559ae5667779fe` is a compare-and-swap): [Image atomics](../../../docs/reference/shaders.md#image-atomics). The whole library was not rebuilt and none of the 5 was requested in the runs above (a missing one prints a `[RED] failed to load shader` line).
- PR #138 (not merged) built them by flattening the coordinate in `dmsh_build.py` (`x + width × (y + height × z)` into the atomics pointer); that is a change of the chain, so every game's libraries would be rebuilt. It is left out until a run of this game requests one of them.

## Movies and audio

- The opening logos are H.264 (1280×720) into an NVN texture with PCM sound, through `subsdk0` ([H.264 into an RGBA texture and AAC through AudioToolbox](../../../docs/reference/movies.md#h264-into-an-rgba-texture-and-aac-through-audiotoolbox)).
- Wwise opens one 6-channel AudioOut. A muted run with `DM_AUDIO_DUMP` had sound under the logos; the language screen is silent.

## Input

- The pad. In the Ghost World the D-pad (or the left stick) stretches the soul toward a core while held; `Right+Up~3` from Sissel's body reaches the crossing gate in the first scene. A tricks.

## Mac routes

- The work folder's `run/boot.sh <log> [secs] VAR=…` runs `build/ghosthost` (a renamed copy of `dmhost`) with the DLC folder; `run/ctl.sh`, `run/step.sh` and `run/see.sh` (a step plus a 640-wide copy of the shot) drive a run started with `DM_CTL`. `run/save_ref` is the save right after the first-boot notices (language English, Bonus Content unlocked, no chapter saved).
- From a fresh save the language screen is up about 35 s after the boot: A (English), A (Next), `LRight~0.2` and A (Next on the privacy notice), A (create save data), A (autosave notice), then about nine A for the Bonus Content notices, which start Chapter 1.
- In Chapter 1: about 63 A through the opening dialogue, `Right~0.3` and `Right+Up~3` to the crossing gate's core, then A through the dialogue and A for the trick.

## iPhone

- Plays (user, IPA 3.0 GB with the autorelease fix). The first IPA (A20 Pro, before that fix) logged the boot, the opening movies and save creation at 60 fps (frame interval p99 about 18 ms), no red line, 4.2 GB of headroom to the system memory limit at frame 1200, and the same freed-but-alive pools as the Mac.

## Memory

- **Resolved: 125 to 146 finalized memory pools stayed alive** (`freed but alive` in the memory line: 6.6 to 8.7 GB of zero-copy buffers, so "GPU allocated" read 9.9 to 11.7 GB against 2.2 GB resident). The game frees large pools while it loads its first scenes, and the host's weak-table bookkeeping of freed pools left each buffer in the finalizing thread's autorelease pool, which never drains ([Finalized pool buffers kept by a guest thread's autorelease pool](../../../docs/reference/nvn-objects.md#finalized-pool-buffers-kept-by-a-guest-threads-autorelease-pool)). Now 0, and GPU allocated 2.9 GB in Chapter 1; resident memory unchanged.

## Found on this game

General host changes first needed here; each applies to every game:

- **Autorelease pools around the pool and residency bookkeeping** (`mtl_pool_del`, `res_add_at`, `res_remove`): [Finalized pool buffers kept by a guest thread's autorelease pool](../../../docs/reference/nvn-objects.md#finalized-pool-buffers-kept-by-a-guest-threads-autorelease-pool).

## Open

- The chapter autosave was not reached in the runs above (the save files kept their first-boot contents through the junkyard tutorial).
