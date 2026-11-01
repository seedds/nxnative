# AI: THE SOMNIUM FILES (`010089B00D09C000`)

Update 1.0.1 (v65536) over the base game, SDK 6.4.2. Unity 2017.4.17f1 IL2CPP with FMOD, on NVN. Modules `main`, `subsdk0` (the movie module) and `sdk`. Plays on the iPhone; 60 fps (present interval 1). The README lists its IPA at about 8.3 GB.

Brought up on the Mac: the Spike Chunsoft logo movie, the title (its background movies), a new game (language, auto save notice), the opening at the merry-go-round and the first investigation with its conversation tutorial, both sticks; loading the autosave from the title. About 2.27 GB resident, flat over 5 minutes; audio through FMOD's AudioOut.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `pad_layout full` | [`game.tsv`](game.tsv) | the 3D gamepad layout: the investigation moves its cursor with the left stick and the view with the right one |
| `mem_used 0x6800000` (104 MB) | `game.tsv` | the rewritten modules take more than the default 64 MB of used memory: with 64 MB the heap request plus the modules exceeds the pool and `SetHeapSize` fails (as Cuphead, [0100A5C00D162000](../0100A5C00D162000/README.md)) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so the game has no virtual address memory |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region; see [Memory](#memory) |

No `patches.py`, and `dm/tools/sdk_patches.py` has no cut for its `sdk` build (`184751379007f9c50fc96ba4d26a5dca`).

## Memory

- No system resource: Unity sets a heap of the application pool − used memory (3.11 GB) in the 4 GB layout and allocates from it. `mem_used` is 104 MB for the rewritten modules.
- On the Mac: 2.27 GB resident over 5 minutes from boot to the first investigation, flat.

## Movies

- `subsdk0` is the movie module. The 168 movies (`StreamingAssets/Movie`, H.264 and AAC in MP4) play through Unity's video player: NV12 frames into CPU buffers, copied to an R8 + RG8 texture pair each frame (`nvnCommandBufferCopyBufferToTexture`).
- **Resolved: the game never left a movie that had played to its end; the last picture stayed on screen.** The 6 s Spike Chunsoft logo (`StreamingAssets/Movie/al_titel_cl_sc_1k_30.mp4`, H.264 High) before the title held the game there. `DM_MOVIE_TRACE=1` showed the end-of-stream packet arriving with H.264 pictures still in the reorder queue: its output went out as a black picture without the end-of-stream flag, and the queued pictures were to follow one per free output slot (the drain). Unity held all 16 video slots by then, and `ReleaseOutputBufferIndex` decoded again only when packets were queued, so the drain never went on and the game waited for the movie's end for good. Fixed in `hle_movie.c` (a released slot goes on with the drain; see [Found on this game](#found-on-this-game)). The audio decoder was never affected: AAC has no reorder queue.

## Rendering

- Docked, the game draws 1280x720 by its own choice and crops the 1920x1080 window to it.
- **Resolved: a draw with the vertex shader of another Shader object.** Once on entering the first investigation: `[RED] failed to create render pipeline (vertex 790573a52b9c5053_b fragment f39fb42850f50ecd_b): Fragment input(s) user(locn0),user(locn1) … not written by vertex shader`. The vertex shader was `Legacy Shaders/VertexLit`'s shadow caster, the pixel shader `PSYNC/BG_AlphaTest`'s, whose own pass pairs it with `00b462fe0dec6e06`. Unity binds the vertex and fragment programs apart, so every draw's pair is a composite of `bind_stages`, one of a ring of 64. In a 280-draw submission the precompile prediction (`pso_predict_rest`) made more than 64 composites and reused the slot the saved `S.prog` named, so the draws after the restore, up to the next bind, ran with that slot's new pair. Fixed by `prog_resolve` (logged as `combined program taken again after a state restore`). Two runs each from one save with one key script, prewarm off: the old host logged `texture type mismatch: shader declares a cube, texture is 2D` both times (a pixel shader run with another pair's bindings), the fixed host no red line, with the slot taken again twice.

## Files and shaders

- The shaders are Shader objects in the AssetBundles of `StreamingAssets` (read by `shader_split.py` through `unityfs.py`, Cuphead's Unity 2017.4 layout) and in `main`: 13,057 (6,572 vertex, 6,482 pixel, 3 geometry). All build for Mac and iOS.
- The ComputeShader objects of `Data/resources.assets` have no type tree (a red line in the split, as in Cuphead); no dispatch has missed one so far.
- `romfs_marker` is `Data/StreamingAssets/AssetBundles/Switch/audio`.

### Geometry shaders

The three geometry shaders were the test case of the general geometry-shader compute path ([geometry support and limits](../../../docs/geometry-metal-research.md#ai-the-somnium-files-evidence)):

| GS key | Effect | Candidate VS |
|---|---|---|
| `1b378c5c014e3a32` | `Hidden/DepthOfField/BokehSplatting`: expands input vertex 0 to a four-vertex triangle strip | `ba2512f7f6973541` |
| `d57cdb168fdf6927` | `Hidden/Dof/DX11Dof`: the same expansion, input varying locations 0 and 1 exchanged | `771d597a6eb545aa` |
| `2341c1be4fcbf690` | `UCLA Game Lab/Wireframe/Single-Sided`: screen-space edge distances from all three positions, three vertices out | `ce9cbd7dca9fda74` |

- The candidate vertex keys come from adjacent entries of the same Unity shader, not a captured runtime binding. The bokeh shaders really expand geometry and fail the layer-only fold; wireframe cannot drop its GS, as its fragment inputs depend on the other two vertices.
- **Current:** they build through the native-VS-capture / GS-compute path. `t_geometry`, built with `GAME=010089B00D09C000` and given the current `cache/b-<chain>` as its first argument, also checks the capture entry and the three translated GS kernels against numeric expectations (498 checks passed on an M4 Pro with Metal API validation). A 90-second Mac run reached the title at 60 fps with no `[RED]` line (its final `CRASH sig=14` is the harness's time limit). Artifacts: `010089B00D09C000-work/geometry-build/` and `geometry-probe/` beside the game's NSPs.
- None of them was drawn in the opening observed so far, so their in-game appearance and cost are not checked.

## Input

- Controller only, docked, with the 3D pad layout: the investigation moves its cursor with the left stick and the view with the right one.

## Mac routes

- The work folder's `run/ctl.sh` drives a run started with `DM_CTL`. From a fresh save: 15 s, A (New Game), Down, Down, A (English), Left, A (Yes), then A every 3 s into the investigation (about 3 minutes).
- With the autosave: `DM_KEYS="15:A,18:A,21:Left,22:A,30:A,40:A*45/3"` loads it and plays the opening again.

## Found on this game

General host changes first needed here; each applies to every game:

- **H.264 drain on a released output slot** (`ReleaseOutputBufferIndex` goes on with the drain; the end-of-stream output takes the first queued picture, `jflush`, `hle_movie.c`): [H.264 drain at the end of a stream goes on with released slots](../../../docs/reference/movies.md#h264-drain-at-the-end-of-a-stream-goes-on-with-released-slots).
- **Per-stage composite taken again after a replay state restore** (`prog_resolve` in `nvn_mtl.m`, after `pso_predict_rest`, the fusion probe and the deferred-draw replay): [Composite program taken again after a replay state restore](../../../docs/reference/rendering.md#composite-program-taken-again-after-a-replay-state-restore).
- **Geometry shaders as compute** (native VS capture, GS dispatch, output-reader vertex function; `t_geometry`): [geometry support and limits](../../../docs/geometry-metal-research.md).
- The spin-wait search's store rule takes one wider loop in its `main`: [development.md, Power](../../../docs/development.md#power).

## Open

- `game.tsv` still says the 3 geometry shaders "do not build"; the development notes and the geometry research say all 13,057 build, including them.
