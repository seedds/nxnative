# Mario Kart World (`0400C3F00006E000`)

A Switch 2 game. Brought up on the base game (v0), SDK 19.3.3 built for the Switch 2 (`sdk` names its spec `Ounce`), NVN2 API 231.16. Nintendo EPD's engine (sead / agl / gsys, Phive, Havok, Pia), modules `main`, `subsdk0` (a Mono runtime, `nn::ecli`) and `sdk`. **Reaches a race on the Mac** (M6, 16 GB): the title, the main menu, the Single Player and Grand Prix menus, character, kart, cup and class selection, the Mushroom Cup's opening and the first race of Mario Bros. Circuit at 50cc, driven with items, at 60 fps on that Mac (see [Frame rate](#frame-rate)). Update 1.8.0 is packed from a profile derived from this one; it is not on this Mac and has not been run (see [1.8.0](#180)). iOS is not tested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x4000000` | [`game.tsv`](game.tsv) | the NPDM declares 64 MB of system resource: virtual address memory on, and `nninitStartup` still sets a heap |
| `app_pool 0x240000000`, `mem_reserve 0x320000000`, `mem_alias 0x100000000` | `game.tsv` | the Switch 2's 9 GB, as Donkey Kong Bananza's. `SetHeapSize` asks for 0x1dbc00000 (7.43 GB; 1.8.0 asks 0x1de400000 by pull request #1116), and with a 7.5 GB heap region the first boot stopped there (`SetHeapSize OOM`): an 8 GB heap region. The alias region is mapped up to +1.75 GB by the title |
| `modules main subsdk0 subsdk1 sdk` | `game.tsv` | the base game has no `subsdk1`; each build row names its own modules ([A version without one of the profile's modules](../../../docs/reference/packing.md#a-version-without-one-of-the-profiles-modules-stops-the-host-at-boot)) |
| `render_cond on` | `game.tsv` | agl wraps its clears in render conditions, as in Donkey Kong Bananza and Splatoon Raiders; not tried without it here |
| `romfs_marker Movie/StaffRoll_00.webm` | `game.tsv` | a file of both versions' RomFS |

## Files

- Packed from a dump folder (`base/` only) with [`scripts/pack_dump.py`](../../../scripts/pack_dump.py): 205,902 RomFS entries, 21.7 GB.
- Shaders: 11,561 in [`shader_map.tsv`](shader_map.tsv) (compute 727, domain 12, pixel 7,261, vertex 3,561), from `Shader/` (bfsha, `Drive.Nin_Ounce_NVN2.release.sarc.zs`), `Lib/` (agl, gsys), `Effect/`, `Layout/` and 40 in `main`.
- The rewriter (`dm/tools/patch_nso.py`) rewrites the three modules with no cut. The SDK-wide address region rules of `dm/tools/sdk_patches.py` match no function of this `sdk`.

## Shaders

The full Mac build translates all 11,561. Getting there took four things, all general:

- 25 are task and mesh shaders (7 task, 18 mesh), which the split used to file as vertex and domain: [NVN2 task and mesh shaders](../../../docs/reference/shaders.md#nvn2-task-and-mesh-shaders). They draw the bushes and grass tufts beside the track.
- 8 pixel shaders use UP2UR with masks 8 and 0x40, and one compute shader (`ab77eaee44cfef88`) SURED: [Ampere shader code](../../../docs/reference/shaders.md#ampere-shader-code-switch-2).
- The 168 compute shaders of `DrivePassShaderDefault` that declare 48 KB of shared memory build since [Compute shared memory past 32 KB](../../../docs/reference/shaders.md#compute-shared-memory-past-32-kb); before it, the 3D scene of the title had sky and clouds over black geometry.
- `dcb0504c844f1ecc` copies a flat per-primitive attribute into an R8Uint target, so it needs the integer entry point `fs_uint` now gives such a shader.

## Rendering

The game draws into the top left 2048x1152 of a 3840x2160 window texture and crops the window to it, so screenshots are taken with `DM_NVN_SHOT_OUT=1920x1080` (`play.sh` in the work folder does).

On the way to the title:

1. Every command buffer failed with `kIOGPUCommandBufferCallbackErrorOutOfMemory` and no frame was presented: six terrain arrays of 8192x8192x68 (two BC6H of 5,803 MB, four ASTC 8x8 of 1,451 MB), made whole, took more than the Mac's memory ([Large sparse textures as Metal placement sparse textures](../../../docs/reference/nvn-objects.md#large-sparse-textures-as-metal-placement-sparse-textures)).
2. Black geometry under the sky: the 48 KB compute shaders' dispatches were skipped ([Shaders](#shaders)).
3. 611 pixel shaders refused at load: [Uniform references past the first kilobyte](../../../docs/reference/shaders.md#uniform-references-past-the-first-kilobyte).

To the race:

4. `CRASH pc=0` from `main+0x19e77a0` when the cup's opening movie starts: `nn::syncpt::sdk::TryWaitSyncPointEvent` went through the SDK's null table, as `CreateSyncPointEvent` did on Donkey Kong Bananza; the host answers it from the event's SystemEvent (`hle_codec_video.c`). The movie player is in `main` and takes SDK 19's decoder forms ([SDK 19's decoder forms](../../../docs/reference/movies.md#sdk-19s-decoder-forms)): VP9 at 2560x1440.

## Frame rate

The first race on the 16 GB M6, in order of the fixes:

| Host | Frame p50 | Frame p95 | Submit |
|---|---|---|---|
| First race | 96-117 ms | about 280 ms | |
| A placement sparse texture no longer copied whole on its first view (389 MB) | about 22 ms | 100-155 ms | 11 ms |
| The sparse pool's change log instead of a walk of the page table per lookup, a cached pool span in the dirty tracker | 16.5 ms | 33-85 ms | 4-6 ms |
| [Large pools wrapped in chunks](../../../docs/reference/nvn-objects.md#large-pools-wrapped-in-chunks) | 16.1-16.7 ms | 17-33 ms | 4-6 ms |
| The task and mesh draws, now that they run | 15.9-17.2 ms | 21-31 ms | 4-6.5 ms |

GPU time is about 8.3 ms a frame without the mesh draws and 8.6 to 9.3 ms with them; the numbers after this are from before them. Over 3,900 frames of the race (frames 8100 to 12000) the worst frame of each 300 is usually 35 to 65 ms, with five of the fourteen stretches reaching 140 to 233 ms; pipelines that compile at first use account for some of them (one took 703 ms in an earlier run), and there is no prewarm list yet. No out-of-memory error came. The residency set is 7.1 GB (shared buffers 1,554 MB, textures 2,011 MB, placement heaps 3,328 MB, private 192 MB), wired memory steady at 9.6 to 9.8 GB, about 6.5 GB of swap in use with 130 MB free. Not measured on a Mac with more memory, or on iOS, where the address space is far smaller.

## Terrain and movies

The flat ground with blocky seams behind the title and the menu was terrain tiles the CPU wrote with no flush, read as zeros: [Uncompressed storage the CPU wrote, in a pool the GPU has not written](../../../docs/reference/nvn-objects.md#uncompressed-storage-the-cpu-wrote-in-a-pool-the-gpu-has-not-written). The title now shows real terrain, with hills, lawns and road edges.

The Mushroom Cup's opening movie plays: VP9 at 2560x1440, 1,423 pictures over about 25 s of frames at steady pacing, with correct colours. It used to show the last 3D frame, which [Textures of an sRGB / linear pair over the same storage](../../../docs/reference/rendering.md#textures-of-an-srgb--linear-pair-over-the-same-storage) fits.

## Driving it on the Mac

The work folder `0400C3F00006E000-work` beside the dump has `rewrite.sh`, `rebuild.sh` (host copy `build/mkwhost`), `split.sh`, `shaders.sh`, `play.sh` (boots headless with `DM_CTL=run/ctl.txt`), `ctl.sh` (control lines; `wait S`, `snap NAME`), `to_race.sh` and `measure.sh <log>`. From a boot at the title (about 80 s): `to_race.sh` presses L+R, then A nine times with waits (Single Player, Grand Prix, Mario, the kart, the Mushroom Cup, 50cc, "Start Grand Prix?"). The opening and the race's start then take about a minute; `A~40` drives. `measure.sh` does all of this, waits for the race (over 1,500 draws a frame, no movie thread), and writes the timing and the `restop` residency lines to `run/<log>.res`. Control presses run in order, so `A~40` holds up later lines.

## 1.8.0

Update 1.8.0 (v917504, SDK 21.4.0) is not on this Mac. Pull request #1116 ported it, and its facts (not its code) went into the host for this profile: the packing of a version with a `subsdk1`, and the Switch 2 services 1.8.0 calls at boot, which the base game does not import (the Nmouse pointer, GameChat's room event and camera storage, the camera context, the graphics system check, the HDR Bezier metadata and its clears; [services.md](../../../docs/reference/services.md#the-camera-through-the-graphics-system)). The NSP script packs it from a profile derived from this one, with its own shader tables. None of it has been run on 1.8.0.

## Open

- **Long frames:** first-use pipeline compiles (no `pso_manifest.tsv` yet).
- **Shading rate:** the calls are accepted and ignored ([Variable rate shading ignored](../../../docs/reference/rendering.md#variable-rate-shading-ignored)), so every pixel is shaded where the game meant to save GPU time.
- **Mesh draws:** `DrawMeshTasks3D` and the indirect forms are not implemented; no run reached them.
- **Vanishing scenery:** whole groups of trees and bushes vanish or appear in one frame while the camera barely moves, in the title scenes and while driving. The instance counts of the draws step together, for example 98 to 35 and 72 to 16 in one frame. Conditional rendering, stale camera uniforms and a depth pyramid in the culling shader are ruled out; the cause is open.
- The engine class screen offers 50, 100 and 150cc in this build.

## Found on this game

- [Large sparse textures as Metal placement sparse textures](../../../docs/reference/nvn-objects.md#large-sparse-textures-as-metal-placement-sparse-textures)
- [Large pools wrapped in chunks](../../../docs/reference/nvn-objects.md#large-pools-wrapped-in-chunks)
- [NVN2 task and mesh shaders](../../../docs/reference/shaders.md#nvn2-task-and-mesh-shaders)
- [Uncompressed storage the CPU wrote, in a pool the GPU has not written](../../../docs/reference/nvn-objects.md#uncompressed-storage-the-cpu-wrote-in-a-pool-the-gpu-has-not-written)
- [Variable rate shading ignored](../../../docs/reference/rendering.md#variable-rate-shading-ignored)
- [Compute shared memory past 32 KB](../../../docs/reference/shaders.md#compute-shared-memory-past-32-kb)
- [Uniform references past the first kilobyte](../../../docs/reference/shaders.md#uniform-references-past-the-first-kilobyte)
- [SDK 19's decoder forms](../../../docs/reference/movies.md#sdk-19s-decoder-forms)
- [A version without one of the profile's modules stops the host at boot](../../../docs/reference/packing.md#a-version-without-one-of-the-profiles-modules-stops-the-host-at-boot)
- 1.8.0, by pull request #1116: [The camera through the graphics system](../../../docs/reference/services.md#the-camera-through-the-graphics-system), [Layer HDR metadata](../../../docs/reference/services.md#layer-hdr-metadata), the SDK 21 forms in [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer) and [GameChat at boot](../../../docs/reference/services.md#gamechat-at-boot)
