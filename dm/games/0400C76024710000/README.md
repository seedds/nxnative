# PRAGMATA (`0400C76024710000`)

A Switch 2 game: update v196608, an SDK built for the Switch 2 (`Ounce`), NVN2 (Ampere shaders). RE Engine: KPKA 4.2 PAKs, the whole 3D scene mesh-shaded through NVN2 task and mesh shaders into a visibility buffer, streamed data in sparse memory pools. Modules `main` and `sdk`. **Reaches its title and plays the New Game opening on the Mac, through the tower-ring cutscene into the station interior** (status 2026-11-05), with the open issues below; it is a work in progress. There is no profile in the repository yet (so no row in the games table) and no IPA has been built.

## What it needs

The profile used for the runs (not in the repository yet) has these rows in its `game.tsv`, among others:

| Setting | Why |
|---|---|
| `mem_reserve 0x270000000`, `app_pool 0x240000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000` | RE Engine's GPU heap is one 64 GB virtual pool built from separate physical pools ([Sparse virtual pools built from separate physical pools](../../../docs/reference/nvn-objects.md#sparse-virtual-pools-built-from-separate-physical-pools)) |
| `romfs_marker re_chunk_000.pak`, `zip_store .pak:.metallib` | the game's PAKs are compressed already |
| `tex_flush_upload on`, `tex_dirty_upload on` | streamed textures written by the CPU |

## Files

- Base, one update (v196608, about 13 GB) and one DLC. The RomFS is RE Engine PAKs, from `re_chunk_000.pak` on.
- `main` build `b83eeb94da35ff07d0cfa955875e8258`, `sdk` `820cd7d096aac448693f504400cc2375`.

## Shaders

- 36,779 in the shader map: 18,406 pixel, 10,829 vertex, 6,880 compute, 539 mesh, 62 domain, 33 hull, 30 geometry. They come from the Switch 2 form of the SDF definitions in the KPKA 4.2 PAKs ([SDF definitions in RE Engine PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks)), plus 139 in the OCIO colour management resources ([Colour management shaders in RE Engine OCIO resources](../../../docs/reference/shaders.md#colour-management-shaders-in-re-engine-ocio-resources)). A program's task and mesh shaders sit in its hull and domain slots.
- The shader compiler is newer than Donkey Kong Bananza's (control +0x08 is 0x11, DKB's 0x0b): the per-stage storage buffer banks 25..29 and bindless constant buffers come with it.
- Useful keys: `5aab38bb5ba67ef7` the title's lunar terrain (mesh) with fragment `6c47597348ba33c1` (writes the visibility buffer); `12a52b683eec95c1` the GPU culling queue that writes the mesh draw records; `5318516d206dba46` the instance cull; `95656724c5b02299` the occlusion pyramid; `f6658e3c09d9600a` with `5367453d98983dab` the tonemap.

## Rendering

- The title matches a published 1080p capture of the same title camera (from another platform): the Earth upper left, the logo to its right, cratered lunar terrain over the bottom third.
- The frame: the GPU culling queue writes `DrawMeshTasksIndirect` records, the mesh shaders draw the visible clusters into an RG32Uint visibility buffer (`(cluster << 7) | primitive` as the primitive id), and later passes shade from it; the OCIO tonemap ends the frame.

## Driving it on the Mac

Through `DM_CTL`, silent (`DM_AUDIO=0`) and with `DM_FOCUS_KEEP=1`: press `A` about once a second through the logos and the title to the difficulty selection, choose one, then the confirm dialog, which opens on No: one `Left` to Yes, then `A`, and keep pressing `A` through the opening's prompts. Run from a copy of a save: the autosave changes the boot path.

## Open

- **Runaway kernels (current).** Three of the game's kernels run loops whose trip count comes from data, and on some inputs they run for minutes, which the GPU watchdog ends (the Orbitals series, #1777: the process exits instead of the Mac's GPU wedging):
  - `26166d3726a4ab4b`, a software rasterizer: at the tower ring it gets screen-sized triangles and walks every pixel of their bounding box;
  - `6a3750228d1d60b2`, a particle emitter: its loop bound is read as a float from a GPU scratch block the game reuses between effects, and on the emitter's first frame (before its own init pass, which runs one pass later) the block holds another effect's floats, about 1e9, so every thread runs to the end;
  - `9af5728334cd12f5`: `for (i = 0; i <= N - 8; i += 8)` with an unsigned N, about 2^29 iterations when N < 8 (found by a static check; its scene is not reached yet).
  - The runs that pass the tower ring use hand-edited libraries of these three (a loop cap, and for the rasterizer a skip of boxes over 4,096 px) through a local shader override directory, not part of the repository. A general loop guard in the translator, off by default, is in progress.
- **Station interior after the tower ring (current):** magenta characters, speckled suits, a stretched streak texture and large dark shards, from the interior's entry on (about 250–290 s). They are already in the G-buffer albedo: 58 % from vertex `5b56bfba` / fragment `fabce160`, 36 % from vertex `dd8edd6e` / fragment `1c9fb5d1`, all indexed indirect multi-draws, whose four shaders check clean statically. Suspect: the host's handling of indexed indirect multi-draw records (stride, count, first index, base vertex, base instance), like the mesh records were.
- **Flyover flashes (current):** in the opening's flyover a patch of lunar ground goes darker (about ×0.68) for one frame, and again 8 frames later, every 1.5 s of game time. It is made by the half-resolution deferred lighting pass before DLSS (fragment `10e0a286118d384e`, 768×432 into the DLSS input colour), on frames locked to the game clock (two frames 0.133 s apart in each 1.5 s cycle), from inputs that look the same as the neighbouring frames': most likely how a time-driven light is shaded on those frames. DLSS (MetalFX's temporal scaler here) only carries it into its history: with DLSS's bilinear stand-in instead the patches stay, and are larger. Other one-frame classes seen: a tablet glare ping-pong, crew-scene brightness flips.
- **Compute kernels skipped:** the runs so far skip 58 compute kernels with a diagnostic skip list that is not in the repository, a safety list from before the MEMBAR and loop guard fixes. Each needs checking with them in place.
- Not checked: gameplay past the opening's interior, audio, an IPA.

## Found on this game

- [SDF definitions in RE Engine PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks): KPKA 4.2 and the Switch 2 SDF form.
- [Colour management shaders in RE Engine OCIO resources](../../../docs/reference/shaders.md#colour-management-shaders-in-re-engine-ocio-resources): every frame black under the UI.
- [LDC with the bank in its address register (`.IS`)](../../../docs/reference/shaders.md#ldc-with-the-bank-in-its-address-register-is) and [A deferred write under a false guard](../../../docs/reference/shaders.md#a-deferred-write-under-a-false-guard): c1 read at a run-time index.
- [Pixel `c0 + 0x620`: the sample index and GLSLC's sample position table](../../../docs/reference/shaders.md#pixel-c0--0x620-the-sample-index-and-glslcs-sample-position-table): signed offsets from the centre on an Ounce SDK.
