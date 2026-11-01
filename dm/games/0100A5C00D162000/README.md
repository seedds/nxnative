# Cuphead (`0100A5C00D162000`)

Update 1.3.7 (v655360) over the base game; its DLC (The Delicious Last Course) is mounted as add-on content 1. SDK 5.5.0, NVN API 53.313. Unity 2017.4.9f1 IL2CPP on NVN. Modules `main`, `subsdk0` (the movie module, `NintendoSDK_movie`) and `sdk`. Plays on the iPhone. On the Mac it was brought up with no host change: the title, the save slots, the opening storybook, Elder Kettle's house, Inkwell Isle One and Forest Follies at 60 fps with no red line. Unity 2017.4's serialized and `Shader` formats (`unityfs.py`, `shader_split.py`) and the rewriter's jump-table form (`patch_nso.py`) were found by armyja on this game (GitHub pull requests #1, #2).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_used 0x5400000` (84 MB) | [`game.tsv`](game.tsv) | the rewritten modules take 80 MB (`main` 59.9, `sdk` 12.7, `subsdk0` 7.3); with the default 64 MB the heap request plus the modules exceeds the pool, `SetHeapSize` fails and the game aborts at its first allocation (a 0.03 GB heap) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource in the NPDM, so no virtual address memory: 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region |
| ZCull storage size never 0 | host, `hle_nvn.c` `zcull_size` (every game) | Unity's NVN pool allocator breaks on a zero-size block; see [Rendering](#rendering) |
| `shader_groups /Data/=data` | `game.tsv` | |

## Memory

- No system resource, so no virtual address memory: Unity sets a heap of the application pool − used memory (3.12 GB) in the 4 GB block and allocates from it.
- On the Mac about 3.2 GB resident and 2.7 GB of Metal allocations, flat over the house and the map.

## Rendering

- **Resolved: sprites broke into dark slivers or went missing in about half the runs** (a log in Forest Follies splintered for a whole attempt), **and on the iPhone the picture flickered every fourth frame.** The host reported a ZCull storage size of 0. Unity's NVN pool allocator allocates each depth target's ZCull save area from its pool; asked for a zero-size block, it gave out the offset of a live block, lost that block from its map of allocations, and later freed it with a garbage size. Buffers allocated afterwards overlapped live ones, so copies read staging bytes another copy had overwritten (every fourth frame is the upload ring's period); whether a run broke depended on the allocation order. The size is now never 0 (`zcull_size`).
- **How it was traced:** the pass dump (`DM_NVN_PASSDUMP_ALL=1`, the `Y copy_b2b` / `Y update_ub` lines) listed in command order every copy and inline update of the frame. A copy whose source an earlier GPU write of the same frame covers reads that write, not the bytes the CPU staged: the broken runs had dozens of those a frame, the clean runs none. The game's allocations overlapped, so the allocator was tracked from observation gates (`OBS` in its `patches.py`) on its free-list push and after its map insert, checking at every event that each block allocated so far was still in the game's map.

## Files and shaders

- 3,118 shaders (1,558 pixel, 1,560 vertex) from the player's serialized files and the AssetBundles of `StreamingAssets`, plus 4 GLSLC outputs in `main`. Every binding decodes.
- Its Unity 2017.4 layout is the one AI: THE SOMNIUM FILES ([010089B00D09C000](../010089B00D09C000/README.md)) reads with. Before Unity 2019.3 a `Shader` keeps one segment per platform, the form Cuphead's trees gave `shader_programs`.
- Its 2 `ComputeShader` objects are only in files without type trees, of hashes no layout covers; the split prints a `[RED]` line and leaves their kernels out.

## Saves

- The game writes a slot when it reaches the map, not when the slot is picked.

## Mac routes

- A fresh save through the title (L+R), a new slot, the storybook and the house to the map (at about 160 s): `DM_KEYS="20:L+R,26:A,32:A,38:A,44:A,50:A,56:A*20/3,120:LRight~4,125:A,127:LRight~3,131:A*10/2,152:LLeft~3,155.5:A"`.
- From that save, across the bridge (its entry is narrow between the posts) into Forest Follies (play from about 68 s; Y shoots): `DM_KEYS="20:L+R,26:A,32:A,35:A,40:LRight~1.5,42:LDown~1.5,45:LRight~3,49:LUp~1,51:LRight~3,54.5:LLeft~0.5,55.2:LUp~0.25,55.6:LRight~2,58:LUp~0.2,58.4:LRight~0.25,58.8:LUp~0.2,59.2:LRight~0.25,59.6:LUp~0.2,60:LRight~0.25,60.4:LUp~0.2,61:A,63:A"`.

## Found on this game

General host changes first needed here; each applies to every game:

- **A size the hardware never reports as 0 is never 0 on the host** (`zcull_size`): [Sizes the hardware never reports as 0 (ZCull save area)](../../../docs/reference/nvn-objects.md#sizes-the-hardware-never-reports-as-0-zcull-save-area).
- **Overlapping copies in the pass dump** as the sign of a broken game allocator: [05, Intermittent rendering glitches](../../../docs/agents/05-debugging.md#intermittent-rendering-glitches).
- **`mem_used`** for rewritten modules over 64 MB, which the NSP script now sets after the rewrite: [dm/games/README.md, Keys](../README.md#keys) and [Adding a game](../README.md#adding-a-game).
- **Unity 2017.4 serialized and `Shader` formats** (`unityfs.py`, `shader_split.py`) and the rewriter's jump-table form (`patch_nso.py`), by armyja: [shaders.md, Containers by engine](../../../docs/reference/shaders.md#containers-by-engine), [Offset jump tables with the bounds check on another register](../../../docs/reference/rewriter.md#offset-jump-tables-with-the-bounds-check-on-another-register).

## Open

- "SDK 5.5.0 (the oldest so far)" in `game.tsv` and docs/development.md is stale: DARK SOULS: REMASTERED (4.5.0) and Mario Kart 8 Deluxe (4.4.0) are older.
- The tracing text in [nvn-objects.md](../../../docs/reference/nvn-objects.md#sizes-the-hardware-never-reports-as-0-zcull-save-area) names observation gates "in its `patches.py`"; the profile has no `patches.py` now.
- `game.tsv` has no `sys_resource` row, so the host reports its default, 32 MB (`game.py`, `hle_svc.c`), although the NPDM declares none and the notes describe the game as having no virtual address memory. Not checked against the game's behaviour.
