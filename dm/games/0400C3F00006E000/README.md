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
- Shaders: 11,560 in [`shader_map.tsv`](shader_map.tsv) (compute 726, domain 12, pixel 7,261, vertex 3,561), from `Shader/` (bfsha, `Drive.Nin_Ounce_NVN2.release.sarc.zs`), `Lib/` (agl, gsys), `Effect/`, `Layout/` and 40 in `main`.
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
| Real terrain and the whole scenery lists | 15.5-16.5 ms | 21-33 ms | 3.7-5.2 ms |

GPU time is about 8.3 ms a frame without the mesh draws and 8.6 to 9.3 ms with them; the numbers after this are from before them. Over 3,900 frames of the race (frames 8100 to 12000) the worst frame of each 300 is usually 35 to 65 ms, with five of the fourteen stretches reaching 140 to 233 ms; pipelines that compile at first use account for some of them (one took 703 ms in an earlier run), and there is no prewarm list yet. No out-of-memory error came. The residency set is 7.1 GB (shared buffers 1,554 MB, textures 2,011 MB, placement heaps 3,328 MB, private 192 MB), wired memory steady at 9.6 to 9.8 GB, about 6.5 GB of swap in use with 130 MB free. Taking the terrain tiles from their storage costs about 8 ms at the 95th percentile on this Mac (rule off in the same build: 17-24 ms) and 0.24 GB of GPU memory, with resident memory the same at 12.8 GB. It is not a leak, but runs of one build vary a lot with the Mac's memory state: another run right after a regression reached 35-56 ms and 13.8 GB resident with 8.5 GB of swap. Not measured on a Mac with more memory, or on iOS, where the address space is far smaller.

On a 16 GB M1 Pro (2026-11-04) the same race kept its frame median at 25 to 28 ms, the GPU taking about 24 ms of it. Before [Physical pages mapped at several places of a placement sparse texture](../../../docs/reference/nvn-objects.md#physical-pages-mapped-at-several-places-of-a-placement-sparse-texture), the placement heaps reached 2.75 GB within three minutes, wired memory 8.7 GB, and the system swapped (frames to 50 ms). With it, the heaps stayed at 1 GB and wired memory at about 7 GB over five minutes, with no swap growth; two Grand Prix in one session (about 30 minutes) played at a race median of 25 to 27 ms, with single frames of 100 to 200 ms now and then.

On the 16 GB M6 (2026-11-04, a host on the older chain `475a55e74858`, the Mac also swapping for another session's game for part of the run), the first race with those shared tiles: 4 heaps (1,024 MB) where the run above had 12 (3,072 MB), resident memory 11.3 GB where it had 12.8 GB, the frame median 16.7 ms and the 95th percentile 17 to 20 ms over 2,400 frames, no out-of-memory line. The run was minutes long, so it does not show that [issue #1193](https://github.com/seedds/nxnative/issues/1193)'s crash after 16 minutes is gone.

## Terrain and movies

The flat ground with blocky seams behind the title and the menu was terrain tiles the CPU wrote with no flush, read as zeros: [Uncompressed storage the CPU wrote, in a pool the GPU has not written](../../../docs/reference/nvn-objects.md#uncompressed-storage-the-cpu-wrote-in-a-pool-the-gpu-has-not-written). The title now shows real terrain, with hills, lawns and road edges.

The Mushroom Cup's opening movie plays: VP9 at 2560x1440, 1,423 pictures over about 25 s of frames at steady pacing, with correct colours. It used to show the last 3D frame, which [Textures of an sRGB / linear pair over the same storage](../../../docs/reference/rendering.md#textures-of-an-srgb--linear-pair-over-the-same-storage) fits.

## Vanishing scenery

Whole groups of trees and bushes vanished or appeared in one frame while the camera barely moved, for hundreds of frames, in the title scenes and while driving. The culling shader (`e04783bda5a52269`) reads a list that `2ce9071c2f22d74f` rebuilds every few frames, and a race in that shader's reset lost entries: [A store only a dispatch's first invocation makes](../../../docs/reference/shaders.md#a-store-only-a-dispatchs-first-invocation-makes). Ruled out on the way: conditional rendering (only some clears), stale camera uniforms, and a depth pyramid in the culling shader (it reads no textures).

## Sky panels

[Issue #1186](https://github.com/seedds/nxnative/issues/1186): large flat panels in front of the sky on many courses, on the base game and 1.8.0. The vertex shader `90a9a728715fd08b` (`DriveRefreshUber.Procedural`) moves its vertices by a 128x128x5 RGBA16F array, whose `y` reached 1,064 in layer 4 and 1,085 in layer 3 here (14,416 in the report), and stayed under 1 elsewhere. It is not a simulation running away: `8f3581eb40d8ce71` adds to a second array that is cleared every frame, and the large values are drawn into that one. They come from the per-layer task and mesh draws, which read [the dummy buffer](../../../docs/reference/shaders.md#task-and-mesh-draws-reading-the-dummy-buffer) instead of the game's data.

Resolved on the base game, on chain `d5fe9ae8bfba` (M6, 16 GB, another session's game running at the same time): at Crown City the array's `y` stays at or below 0.9 and the sky shots show no panels, and desert scenes checked with an earlier host that remapped the slots show none either. The first race, on the route of `measure.sh`, keeps a frame median of 16.6 to 16.7 ms and a 95th percentile of 17 to 18 ms, with GPU time 7.2 to 7.7 ms and resident memory 11.4 GB. That matches runs from before the fix, so reading real data costs nothing measurable there. One run reached 14 to 15 ms of GPU time and did not repeat. 1.8.0 has not been run here.

## Driving it on the Mac

The work folder `0400C3F00006E000-work` beside the dump has `rewrite.sh`, `rebuild.sh` (host copy `build/mkwhost`), `split.sh`, `shaders.sh`, `play.sh` (boots headless with `DM_CTL=run/ctl.txt`), `ctl.sh` (control lines; `wait S`, `snap NAME`), `to_race.sh` and `measure.sh <log>`. From a boot at the title (about 80 s): `to_race.sh` presses L+R, then A nine times with waits (Single Player, Grand Prix, Mario, the kart, the Mushroom Cup, 50cc, "Start Grand Prix?"). The opening and the race's start then take about a minute; `A~40` drives. `measure.sh` does all of this, waits for the race (over 1,500 draws a frame, no movie thread), and writes the timing and the `restop` residency lines to `run/<log>.res`. Control presses run in order, so `A~40` holds up later lines.

## 1.8.0

Update 1.8.0 (v917504, SDK 21.4.0) is not on this Mac. Pull request #1116 ported it, and its facts (not its code) went into the host for this profile: the packing of a version with a `subsdk1`, and the Switch 2 services 1.8.0 calls at boot, which the base game does not import (the Nmouse pointer, GameChat's room event and camera storage, the camera context, the graphics system check, the HDR Bezier metadata and its clears; [services.md](../../../docs/reference/services.md#the-camera-through-the-graphics-system)). The NSP script packs it from a profile derived from this one, with its own shader tables. None of it has been run on 1.8.0.

## Open

- **Long frames:** first-use pipeline compiles (no `pso_manifest.tsv` yet).
- **Shading rate:** the calls are accepted and ignored ([Variable rate shading ignored](../../../docs/reference/rendering.md#variable-rate-shading-ignored)), so every pixel is shaded where the game meant to save GPU time.
- **Mesh draws:** `DrawMeshTasks3D` and the indirect forms are not implemented; no run reached them.
- `649e36162cad9e79`, a foliage shader with an 8x8 local size, keeps a first-invocation race: its setup loops over a uniform's count, so [the host cannot make it as stores](../../../docs/reference/shaders.md#a-store-only-a-dispatchs-first-invocation-makes). Its list's count read 0 in every one of 7,350 traced title frames, so the race had nothing to act on there; whether that list should be empty is not known.
- The engine class screen offers 50, 100 and 150cc in this build.

## Found on this game

- [A NUP with complete standalone updates stops at "no base game"](../../../docs/reference/packing.md#a-nup-with-complete-standalone-updates-stops-at-no-base-game) (packing reproduced with the base dump; 1.8.0 gameplay remains untested)
- [Large sparse textures as Metal placement sparse textures](../../../docs/reference/nvn-objects.md#large-sparse-textures-as-metal-placement-sparse-textures)
- [Physical pages mapped at several places of a placement sparse texture](../../../docs/reference/nvn-objects.md#physical-pages-mapped-at-several-places-of-a-placement-sparse-texture)
- [Large pools wrapped in chunks](../../../docs/reference/nvn-objects.md#large-pools-wrapped-in-chunks)
- [NVN2 task and mesh shaders](../../../docs/reference/shaders.md#nvn2-task-and-mesh-shaders)
- [Task and mesh draws reading the dummy buffer](../../../docs/reference/shaders.md#task-and-mesh-draws-reading-the-dummy-buffer)
- [A store only a dispatch's first invocation makes](../../../docs/reference/shaders.md#a-store-only-a-dispatchs-first-invocation-makes)
- [Uncompressed storage the CPU wrote, in a pool the GPU has not written](../../../docs/reference/nvn-objects.md#uncompressed-storage-the-cpu-wrote-in-a-pool-the-gpu-has-not-written)
- [Comparisons with .FTZ](../../../docs/reference/shaders.md#comparisons-with-ftz)
- [Variable rate shading ignored](../../../docs/reference/rendering.md#variable-rate-shading-ignored)
- [A DualShock 4 that connects after the first stays on Core Haptics](../../../docs/reference/services.md#a-dualshock-4-that-connects-after-the-first-stays-on-core-haptics)
- [Scaled shared memory addresses](../../../docs/reference/shaders.md#scaled-shared-memory-addresses)
- [iOS ends the app once the guest's touched pages reach its memory limit](../../../docs/reference/memory.md#ios-ends-the-app-once-the-guests-touched-pages-reach-its-memory-limit)
- [Compute shared memory past 32 KB](../../../docs/reference/shaders.md#compute-shared-memory-past-32-kb)
- [Uniform references past the first kilobyte](../../../docs/reference/shaders.md#uniform-references-past-the-first-kilobyte)
- [A control section's words read as a BAHS entry](../../../docs/reference/shaders.md#a-control-sections-words-read-as-a-bahs-entry) (resolved, [issue #1592](https://github.com/seedds/nxnative/issues/1592)): `cs_lib.bin` 74, `797e77c2ec4341c5` at `agl_common.sharcb#BAHS@0x5c778`, was no shader. Dropping it and its fact moved the 69 `cs_lib.bin` rows after it (`agl_common`, `agl_ndsr`, `agl_technique_pfx`, gsys `common`) down one index, as the reporter's split of the base game has them; not split again here, as the game's files are not on this Mac.
- [SDK 19's decoder forms](../../../docs/reference/movies.md#sdk-19s-decoder-forms)
- [A version without one of the profile's modules stops the host at boot](../../../docs/reference/packing.md#a-version-without-one-of-the-profiles-modules-stops-the-host-at-boot)
- 1.8.0, by pull request #1116: [The camera through the graphics system](../../../docs/reference/services.md#the-camera-through-the-graphics-system), [Layer HDR metadata](../../../docs/reference/services.md#layer-hdr-metadata), the SDK 21 forms in [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer) and [GameChat at boot](../../../docs/reference/services.md#gamechat-at-boot)
