# OCTOPATH TRAVELER (`010057D006492000`)

Update 1.0.5 (v327680) over the base game, SDK 3.5.1 (its own `sdk` build; NVN API 53). Square Enix / Acquire, Unreal Engine 4.18 (`++UE4+Release-4.18`; its project is called Kingship): two legacy version 4 paks with the shaders inline in the cooked packages, NVN, PhysX 3.4, CRIWARE (ADX2 sound banks, one USM movie). Modules `main`, `subsdk0` (the movie module) and `sdk`. 60 fps (30 on the Switch).

Status: brought up on the Mac: the boot, the language notice, the title, a new game, the traveler selection (the world map, the travelers' cards), Ophilia's opening (her room, the cathedral hall and nave, the tutorials) and H'aanit's (the flashback, free play in S'warkii), at 60 fps since 2026-11-09 (frame time p50 16.7 ms; 30 before, p50 33.4 ms) with no red line but the two untranslated compute shaders below; about 2.4 GB resident with 1.2 GB of Metal allocations. Not yet seen: a battle, the sound (runs were headless, `DM_AUDIO` off), the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`; `sys_resource 0x0` | [`game.tsv`](game.tsv) | no virtual address memory: `nninitStartup` sets a heap of pool − used (3.08 GB); see [Memory](#memory) |
| `mem_used 0x8000000` | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory |
| `tex_flush_upload on`, `tex_dirty_upload on` | `game.tsv` | the CPU loads textures straight into CPU-uncached pools after creating them and never flushes; see [Rendering](#rendering) |
| `pad_layout full` | `game.tsv` | menus and the battle commands take the D-pad |
| `shader_groups GlobalShaderCache=global #Engine/=engine #Kingship/=game` | `game.tsv` | the global shader cache, the engine's materials and the game's |
| `fps_target 60` | `game.tsv` | 60 fps: the host writes the frame limit the game sets to 30; see [Frame rate](#frame-rate) |

## Frame rate

- **Current:** 60 fps (30 on the Switch), since 2026-11-09. The game's config sets Unreal's frame limit `t.MaxFPS` to 30; its present interval, `nvn.SyncInterval` in UE 4.18's Switch RHI, is already 1 (the log's "present interval set by the game 1"). The rewriter finds both console variables by name (`dm/tools/ue_cvar.py`: `t.MaxFPS` at `main+0x50b5688`, `nvn.SyncInterval` at `main+0x5adc4a8`), and `hle_fpslock.c` writes 60 into `t.MaxFPS` (profile key `fps_target 60`); `DM_FPS_TARGET=30` keeps the game's 30. This build registers its variables in inlined static constructors, which take one page base in `x22` (`main+0x1bfd0a4`, about 700 instructions before the `t.MaxFPS` name at `main+0x1bfdb90`) and store each object's vtable and value pointer at offsets from it (`[x22, #0x5d8]`, `[x22, #0x5e8]`); the search follows that form since 2026-11-09 ([Unreal's frame limit and present interval, found by name](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name)).
- **Measured** (M4 Pro, headless, the S'warkii route below, 300 s each): frame time p50 16.7 ms at 60 against 33.5 ms with `DM_FPS_TARGET=30`, GPU p50 about 7 ms. Long frames (CPU-side: no new pipelines, the GPU under 12 ms) are more frequent at 60: 284 over 50 ms and 101 over 100 ms, against 127 and 39 at 30 in the same 300 s (open, below). Unreal steps the game by the time that really passed, as in [OCTOPATH TRAVELER II](../0100A3501946E000/README.md); the game's speed at 60 has not been compared on a walk yet.

## Memory

- **Current:** the NPDM declares no system resource, so there is no virtual address memory and no engine address region: `nninitStartup` (`main+0x2c67cc` calls into the SDK's heap setup) sets a 3.08 GB heap, and Unreal's own allocator ("Using Standard Allocator", Binned Small Heap 3154 MB) and the NVN pools live in it. The game prints its memory setup through `nn::diag::LogImpl`, which reaches the host log with `DM_DBG=1` (`svcOutputDebugString`), as does an Unreal assertion.
- **Resolved: `SetHeapSize OOM: req 0xc5400000 > cap 0` and a `Break` in `nninitStartup`** with the provisional profile, which gave every Unreal game a 64 GB alias region and no heap region: [Unreal's engine address region](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options) (note).
- **Resolved: `Failed to allocate 1879113728 bytes from Standard Allocator`** at boot: [The sparse tile layout of a texture](../../../docs/reference/nvn-objects.md#the-sparse-tile-layout-of-a-texture).
- On the Mac the process holds about 2.4 GB (1.2 GB of Metal allocations) in the cathedral and in S'warkii; 221 memory pools.

## Rendering

- **Resolved: the language notice's pictures, the title logo and the whole traveler selection missing** (only the backdrop and the footer text drew). Unreal writes these textures from the CPU into CPU-uncached pools (flags 0xe2) after `nvnTextureInitialize`, with no flush, so the host kept them as GPU textures with no content; Slate then drew nothing for them. The profile turns on `tex_flush_upload` and `tex_dirty_upload` ([dm/games/README.md](../README.md)), which upload them from guest memory and again when their pages change.
- **Resolved: the world map on the traveler selection a patchwork of rectangles:** [Sparse textures take the block height of their 64 KB tile](../../../docs/reference/rendering.md#sparse-textures-take-the-block-height-of-their-64-kb-tile).
- **Resolved: streamed textures black or missing** (S'warkii's ground, plants and props) and `[RED] MapVirtual: non-contiguous mapping (true sparse) not implemented`: [Virtual pools of one texture each](../../../docs/reference/nvn-objects.md#virtual-pools-of-one-texture-each-mapped-before-and-after-the-texture).
- **Current (checked, not confirmed on hardware): a dark red cast at the screen edges in S'warkii.** The tonemapper is right (checked by snapshots of its inputs, `DM_NVN_TEXSNAP`): its scene input is neutral there and its LUT turns dark greys red. The LUT is computed every frame from the area's grading parameters (pass with `172ec899e321a2a5`, no textures), which in S'warkii hold a red-weighted gain (1.5, 1.08, 1.0), an offset (1, 0, 0) with −0.116 and a white balance of 8000 K (6500 K on the title), so the strong vignette (intensity 1.17) darkens the edges into the red shadows. The grid there is the out-of-focus foreground foliage's pixel-art blocks.
- Two compute shaders of the global shader cache did not translate (`327fce53f1fce4c1`, `6d87d48e86bd7f3c`: image atomics whose result is used, refused on MSL 2.3), and their dispatch was skipped with a red line. Both build on chain `0ce4b5e8aefb` (a build of those two keys, 2026-10-27; [Image atomics](../../../docs/reference/shaders.md#image-atomics)); not run since.
- 5,434 shaders (4,517 pixel, 672 vertex, 232 compute, 13 geometry), all but those two built for the Mac; all 5,434 built for iOS.

## Files and shaders

- `Kingship/Content/Paks/Kingship-Switch.pak` (1.97 GB, 103,018 files; the profile's `romfs_marker`) and the update's `Kingship-Switch_0_P.pak` (601 MB, 1,294 files), legacy version 4, Zlib blocks, index not encrypted; beside them 383 CRIWARE `.awb` banks, `CRIWARE/movie01_op.usm` and the vibration data. `UE4CommandLine.txt` holds the project path.
- The shaders are inline in the cooked packages and the global shader cache, as for [DRAGON QUEST XI S](../01006C300E9F0000/README.md) ([Unreal Engine 4.18 shaders inline in cooked packages](../../../docs/reference/shaders.md#unreal-engine-418-shaders-inline-in-cooked-packages)); here the paks are Zlib, so no game decoder is needed.

## Rewriter

- **Resolved: `jump table 1676f58`:** [64-bit bounds check of the index](../../../docs/reference/rewriter.md#64-bit-bounds-check-of-the-index).

## Sound

- **Resolved: `FATAL audren: missing SDK symbol _ZN2nn2os12SdkMutexType4LockEv`:** [No `SdkMutexType` in SDK 3](../../../docs/reference/audio.md#no-sdkmutextype-in-sdk-3). The renderer then runs at 32 kHz with the SDK 4 command layout (`DeviceSink` as command 0xf); not listened to yet.

## Mac routes

The work folder `<NSP folder>/010057D006492000-work/run` has `host.sh` (builds the octo1 worktree's host and copies it to `build/o1host`; since 2026-11-09 the NSP script's build holds only the current chain's libraries, so run its `tools/dmhost` with `DM_GAMES_DIR` set to this repository's `dm/games` and `DM_ROMFS=<build>/extract/v327680/RomFS.img`), `boot.sh <log> [secs] VAR=…`, `ctlrun.sh` / `ctl.sh` / `step.sh` / `see.sh` / `mash.sh` for driving a run through `DM_CTL`, `stop.sh`, `to_story.sh` (Ophilia's opening) and `ab.sh` (several variants at once on copies of the save, the same scripted route, shots at given frames).

- With system data in the save (the language notice is shown only on a fresh save): the title at about 28 s; A, A (New Game) opens the traveler selection; L picks a traveler (L once Ophilia, twice H'aanit); A opens the confirmation, whose cursor appears only after a direction press (Down, Up), then A starts the story.
- Into S'warkii: `DM_KEYS=28:A,36:A,70:L,72:L,75:A,78:Down,79:Up,80:A,95:A*480/0.6,390:B` (the village at about frame 12,000).
- In S'warkii: from the start, `LUp~1.5`, `LRight~0.1`, `A` talks to the villager on the path ("This is S'warkii, …"); the headman (green bubble, by the big house) starts the Linde scene and the Path Actions tutorial, after which the objective is to Provoke the two fledgling hunters in the plaza (Y facing one; with nobody in front Y offers only Examine, the monster roster). Characters do not block H'aanit: walking into a villager walks through it, and talking needs her to stop beside one, facing it.
- **Checked, not a bug: walking through villagers.** Probes on PhysX's query callback (`MultiQueryCallback::invoke`, the scene's pre-filter at `main+0x154d750`) show the player's movement sweep (Pawn channel, block bits `fffffef3`, touch bits `4`) only touches Pawn shapes, by the game's own collision data, while villagers' own sweeps block each other and the WorldDynamic queries that touch Pawns find the villagers' capsules.
- The game saves no story progress before its first save point; no save of the openings is kept yet.

## Found on this game

General changes first needed here; each applies to every game:

- **Rewriter:** a 64-bit bounds check of a jump table's index: [64-bit bounds check of the index](../../../docs/reference/rewriter.md#64-bit-bounds-check-of-the-index).
- **Provisional profiles:** Unreal's 64 GB alias layout only with virtual address memory (`profile_guess.py`): [Unreal's engine address region](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options).
- **NVN:** `nvnTextureBuilderGetSparseTileLayout` / `nvnTextureGetSparseTileLayout`: [The sparse tile layout of a texture](../../../docs/reference/nvn-objects.md#the-sparse-tile-layout-of-a-texture); every virtual pool with a range of its own until its first mapping decides: [Virtual pools of one texture each](../../../docs/reference/nvn-objects.md#virtual-pools-of-one-texture-each-mapped-before-and-after-the-texture); sparse textures' block height: [Sparse textures take the block height of their 64 KB tile](../../../docs/reference/rendering.md#sparse-textures-take-the-block-height-of-their-64-kb-tile).
- **Audio:** the update lock without `SdkMutexType`: [No `SdkMutexType` in SDK 3](../../../docs/reference/audio.md#no-sdkmutextype-in-sdk-3).
- **Frame rate:** Unreal console variables registered by inlined static constructors, from a page base made far before the name: [Unreal's frame limit and present interval, found by name](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name).

## Open

- A battle, the sound and the iPhone are not tested; the IPA (3.6 GB) is built but not run on a device.
- The travelers' signatures on their cards stop part-drawn ("Opli…", "Haa…"): not looked into.
- The two untranslated compute shaders (above).
- At 60 fps: CPU-side long frames are 2 to 2.5 times as frequent as at 30 over the same route ([Frame rate](#frame-rate)), not looked into; and the game's speed at 60 is not compared on a walk yet.
