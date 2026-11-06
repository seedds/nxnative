# The Legend of Zelda: Ocarina of Time (`0400486024DEC000`)

A Switch 2 game: update v131072, SDK 22.2.8 (`Ounce`). Nintendo EPD's engine (sead, agl, gsys, Havok, CRIWARE; project `Owl`, with Tears of the Kingdom's file layout) on NVN2. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`, no rewriter patches. Status: plays on the Mac through the title, the opening, a new file's first scene, Link's house and Kokiri Forest, with saves, at about 59 fps with the picture close to console video. Mac only so far.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x320000000`, `mem_alias 0x100000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the game asks `SetHeapSize` for 0x18ce00000; the rest as Donkey Kong Bananza, the Switch 2's 9 GB |
| `mem_map_limit on` | [`game.tsv`](game.tsv) | a map past the application pool fails with ResultLimitReached, as on Horizon. The SDK's allocator keeps freed pages mapped until a map fails, then gives them back ([PR 1444](https://github.com/seedds/nxnative/pull/1444) reports a black screen leaving the Deku Tree without it; not reached here) |
| `sys_resource 0x3000000` | `game.tsv` | 48 MB, as the NPDM declares |
| `render_cond on` | `game.tsv` | agl clears a copy of the prepass depth again under a render condition the console skips. Run unconditionally, the opaque pass's `EQUAL` test fails everywhere ([PR 1299](https://github.com/seedds/nxnative/pull/1299)'s finding) |
| `gpu_timestamps segment` | `game.tsv` | the game's dynamic resolution decides from GPU timestamps, and with one time per submission it never lowers its resolution ([why](../../../docs/reference/frame-rate.md#dynamic-resolution-that-never-moves-and-draw-passes-that-wait-for-each-other)) |
| `draw_barriers game` | `game.tsv` | about 180 full barriers a frame NVN does not ask for: GPU 25 → 18 ms at full resolution |
| `tex_flush_upload on`, `tex_dirty_upload on` | `game.tsv` | as Tears of the Kingdom: the CPU writes the terrain tiles' height (R32F), blend and hole (RGBA8) maps into textures after creating them. Without the keys those textures stay zero: flat blue holes in the ground around the path with grass floating in them, and grey stone where the console shows dirt |
| `shader_groups`, `shader_note` | `game.tsv` | shaders grouped as `/Shader/`, `/Effect/`, `/Lib/`, `/UI/` |
| `pad_layout full` | `game.tsv` | a 3D field and both sticks |

## Files

One [NUP container](../../../docs/development.md#the-packing-script-in-detail) of 25.6 GB: `scripts/pack_nup.py` unpacks it to `extract/v131072/`. The RomFS keeps most files as zstd with dictionaries from `Pack/ZsDic.pack.zs`. The movies are AV1.

## Shaders

- About 42,850 in [`shader_map.tsv`](shader_map.tsv), including 500 mesh and 20 task shaders in the BNSH programs ([Task and mesh shaders in BNSH](../../../docs/reference/shaders.md#task-and-mesh-shaders-in-bnsh-and-indirect-mesh-draws)), and the movie library's in an NDVE pack ([`nn::ndve`](../../../docs/reference/shaders.md#nnndves-movie-enhancement-file)).
- The runs here used a partial Mac library of the keys the playtests asked for, listed in [`shader_seen.tsv`](shader_seen.tsv) (7,276; building them took about 10 minutes when most came from the previous chain's libraries).
- Current: [`RED.F32.FTZ.RN`](../../../docs/reference/shaders.md#float-atomics-in-storage-memory) translates. The build's `Attribute offset 0x5C is not valid` (about 2,300 shaders) is a vertex shader's write to an output the translator drops, probably a shading rate, as `nvnCommandBufferBindShadingRateImage` and its family are no-ops: everything shades at full rate, which is right to look at and costs GPU time. Not verified against the hardware.

## Frame rate

Measured on an M6 Mac with 16 GB, headless, on one walk with camera turns (`walk.sh` in the work folder).

- Current: the title and menu, a movie behind them, are a locked 60 fps (p50 16.67 ms, GPU span 15.6 ms).
- Current: walking the village at the game's top internal resolution, 1536x864: a locked 60 fps, mean 16.67 ms over each settled window. Per 2,700 frames: 0-8 frames over 25 ms and 0-2 over 33 ms. Frame p95 17.8-18.9 ms with no window, as the game's own loop moves around its fence wait; in a window p95 17.8 ms, 3 over 25 ms and 5-16 over 18 ms per 600. GPU span p50 16.5 ms, summed GPU 15.8 ms, CPU 6 ms, about 101 passes a frame. The heavy stretch where the village opens (about 1,560 draws) holds 16.65-16.8 ms by lowering its resolution, then returns to full. The main pass used to sit at 763x429 at the same GPU time: [marked shaders tested depth late](../../../docs/reference/frame-rate.md#a-hosts-own-store-in-a-fragment-shader-moves-its-depth-test).
- Before [pool shaders stopped counting as depth readers](../../../docs/reference/rendering.md#pool-shaders-taken-as-depth-readers): passes 118, 104, 219, 224 in a four-frame cycle, frame times 15, 15, 18, 18.5 ms, mean 16.9 ms. Half the frames rendered the reflection probe's faces with one draw a pass.
- The host's GPU figure adds up command buffers, which overlap, so it reads above the real span: 17.5 ms was never a frame's real time. The real span is `@gpu_end_ms − @gpu_beg_ms` in `DM_NVN_TSV`, or the `first start to last end` of `DM_NVN_GPUPROF`.
- The roughly 1 ms the [wind fix](../../../docs/reference/shaders.md#dead-code-removal-dropped-a-coordinate-of-a-multi-output-instruction) seemed to add: 0.66 ms of it is the upscaler, 416 → 1076 µs, because its variants then read what they had lost. It is doing its real work.
- Attachment traffic was not the cost it looked: 254 MB load and 277 MB store remain, and the cheapest passes move 20 MB in about 30 µs. The pass count was the cost.
- Resolved: 25-40 ms frames while the GPU sat idle with the next command buffer committed, 51-75 per 2,700 in a bad run: [the GPU idles with the next command buffer already committed](../../../docs/reference/frame-rate.md#the-gpu-idles-with-the-next-command-buffer-already-committed). What holds Metal's submission queue in the kernel is still unknown.
- The title movie's network takes 15.4 ms of compute a frame here, so the title holds 60 fps with almost nothing to spare, and slower GPUs fall below it: [issue 1375](https://github.com/seedds/nxnative/issues/1375), 35 ms on an M1 Max.
- In a run that takes shots, each shot costs its frame 60–100 ms, and `DM_NVN_GPUPROF` and pass dumps slow the frames they cover.
- Open: frames of 100–650 ms when an area first loads (640 first uploads, 321 MB, in one frame of 294 ms; another 640 ms of submit with 68 MB).
- `DM_NVN_TBDR=1` was tried and does not help: p95 19 ms on the same walk, against 17.8.
- The first long run on a new chain is slower (p50 60–114 ms), while `MTLCompilerService` compiles pipelines; the next run is not.
- With a window, the first start on a new chain runs the pipeline prebuild first: 33,190 shader pairs, 691 s on this Mac. A run with no window skips it.
- The opening movie runs at the movie's own 24 fps pace during the story section and cannot be skipped: about 8 minutes on a new file.

## Memory

- Current: on the walk, run from an external drive with a 3.5 GB pipeline archive: resident 10.9-11.4 GB, wired about 9.4 GB, on a 16 GB Mac. `vmmap` and `footprint`: 6.4 GB of guest memory and 3.8 GB of Metal textures and targets, 1 GB of IOAccelerator memory. [Reading the archive into memory](../../../docs/reference/memory.md#a-pipeline-archive-on-an-external-drive-is-read-into-memory) used to add 3.5 GB (footprint 15.0, peak 16.7). `mem_map_limit on`: a map past the application pool fails as on Horizon, and the SDK's allocator then gives back the pages it keeps. Other apps' memory adds to the swap: other games running beside it can push the Mac over.
- This game took the Mac down before the fixes (kernel panic in `IOGPUGroupMemory` at 14.7 GB wired): [raw pool table](../../../docs/reference/memory.md#memory-pools-nothing-gave-an-address-to-stay-out-of-the-raw-pool-table), [textures made only when read](../../../docs/reference/memory.md#shaders-report-which-textures-they-read-through-a-pool), [out-of-memory exit](../../../docs/reference/memory.md#repeated-gpu-out-of-memory-failures-end-the-game).
- Why so much: the game's own memory is about the console's 9 GB application pool, and on the Mac its textures also exist as Metal textures, as Metal cannot sample the guest's block-linear storage.
- Open: render targets that move in the game's pool and change size with its dynamic resolution (1580 → 1579 → 1578 …) get a new Metal texture each time, about 2 GB added to the residency set per 300 frames. It costs about 0.5 ms a frame once settled, so recycling them is a smaller win than the walk, but [a report of a GPU driver panic](https://github.com/seedds/nxnative/pull/1299) lines up with this churn.

## Saves

The game autosaves into `slot_00` and `slot_01`, with `option.sav` and `account.sav`. The Load screen lists them and reloading works. A save's thumbnail is a 543x306 JPEG in `caption.sav`, made from a copy of a texture to a buffer. A manual save (Menu, System, Save) on the current build has the picture (slot 02, mean colour 138 / 123 / 86). The autosaves in the work folder's saves, made on older chains, hold a black one. Open: an autosave made on the current build has not been looked at.

## Driving it on the Mac

Run headless, `DM_CTL` at a control file. The work folder has `boot.sh <secs> <log>` (with a guard that kills the host past 12.5 GB wired or 16 GB of swap; do not run without it), `ctl.sh`, `seq.sh`, `snap.sh`, `keys.sh <logs>` (adds and builds the keys the logs ask for), `white.py` (share of near-white pixels in a shot) and `run/save_ref` (autosaves in Kokiri Forest).

From an empty save: at about 45 s `A` (title), `A` (New Game), then the movie. From `save_ref`: at about 58 s `A~0.2`, 5 s later `A~0.2` (Load), 30 s later three `A~0.1` 3 s apart, then 12 s to the bridge outside Link's house. There `LUp~3.5`, `A~0.2` (Climb Down) and `LDown~4` three or four times reach the village floor.

## Open

- Open: 2 of about 42,850 shaders do not compile (`1f78730053feb036`, `e172bd7ec218ab8e`: `bracket nesting level exceeded maximum of 256`). Neither has shown up in a run so far.
- Open: [the movie network](https://github.com/seedds/nxnative/issues/1375) takes 35 ms a frame on an M1 Max, so movies and the title run at about 28 fps there. It fits on this M6, with about 1 ms to spare.
- Current: against a frame of console video of Navi's close-up the picture matches: hair and sign within 1–2 % in red and green. The causes, each general: [float atomics](../../../docs/reference/shaders.md#float-atomics-in-storage-memory) (too bright, yellow skin, a pale veil: the exposure read back from buffers that stayed 0), the terrain's CPU-written maps (holes, wrong materials; the profile keys above), [the gather offsets](../../../docs/reference/shaders.md#texturegatheroffsets-and-a-loop-over-its-offsets) (the dotted rows in blurred areas, born in the NDSR upscaler, three pictures from one input), and [image atomics on RGBA8](../../../docs/reference/shaders.md#image-atomics-on-a-32-bit-colour-texture) (the anti-aliasing's blend weights).
- Current: the grass no longer flickers. Its blades shifted at 10 Hz because the wind's pressure solve read one neighbour at an undefined row ([dead-code removal](../../../docs/reference/shaders.md#dead-code-removal-dropped-a-coordinate-of-a-multi-output-instruction)), so the velocity field (512x512 RGBA16F, a two-buffer ring) held strong stripes up to ±11 with a 6-frame cycle. In the moss on the bridge, the mean difference between adjacent frames fell from 5.97 to 0.70 of 255, and it now grows smoothly with the lag.
- Open, small differences from that same console frame: Navi's core has a harder edge and a narrower glow here (the bloom texture peaks at 0.44; the ball's HDR is about 0.9 with a tiny spot at 45); shadows are less blue, and the far grass about 1.4 times brighter. One frame of compressed video is weak evidence for either. The falling leaves look dark near the camera, not compared.
- A red `texture type mismatch` on `9b310b3349825e9a` (a UI merge shader): a 3D texture left bound in a slot its active branch does not sample. Harmless so far.
- `nn::codec AV1: a packet refused with 12 pictures waiting` (4 lines a run): the decoder's queue is full and the game retries; movies play.
- The `pipeline archive` red line is [issue 342](https://github.com/seedds/nxnative/issues/342), a limit of this system build. `c1_full.tsv is stale` holds on partial libraries, which no full build has stamped.
- The "Navi" name label in the Great Deku Tree scene was missing on an earlier chain and has not been checked since. Not played yet: the Deku Tree dungeon, combat, later areas. Sound opens with no red line; movies and speech were not listened to. Physics issues reported in PR 1299 were not seen on this route (walking, the ladder, the camera).
- No iOS shader build or iPhone run has been done. Its memory use makes an iPhone unlikely as it stands.

## Found on this game

- [The GPU idles with the next command buffer already committed](../../../docs/reference/frame-rate.md#the-gpu-idles-with-the-next-command-buffer-already-committed): the 25-40 ms frames.
- [Pool tables and readback buffers made anew each frame](../../../docs/reference/memory.md#pool-tables-and-readback-buffers-made-anew-each-frame): about 1,500 residency changes per 300 frames.
- [Pool shaders taken as depth readers](../../../docs/reference/rendering.md#pool-shaders-taken-as-depth-readers): about 80 extra passes a frame, and half the frames over budget.
- [A pipeline archive on an external drive is read into memory](../../../docs/reference/memory.md#a-pipeline-archive-on-an-external-drive-is-read-into-memory): 3.5 GB of swapped memory, now mapped.
- [A texture in a sparse pool came out CPU-written at its first use](../../../docs/reference/memory.md#a-texture-in-a-sparse-pool-came-out-cpu-written-at-its-first-use): found by the unit test, on this profile's `tex_dirty_upload`.
- [`EventFd` needs no network](../../../docs/reference/services.md#eventfd-needs-no-network)
- [No virtual audio input devices (SDK 22)](../../../docs/reference/audio.md#no-virtual-audio-input-devices-sdk-22)
- [`nn::ndve`'s movie enhancement file](../../../docs/reference/shaders.md#nnndves-movie-enhancement-file)
- [Task and mesh shaders in BNSH, and indirect mesh draws](../../../docs/reference/shaders.md#task-and-mesh-shaders-in-bnsh-and-indirect-mesh-draws)
- [F2IP](../../../docs/reference/shaders.md#f2ip)
- [A shader that writes a whole frame through raw global memory](../../../docs/reference/frame-rate.md#a-shader-that-writes-a-whole-frame-through-raw-global-memory)
- [Memory pools nothing gave an address to stay out of the raw pool table](../../../docs/reference/memory.md#memory-pools-nothing-gave-an-address-to-stay-out-of-the-raw-pool-table)
- [Shaders report which textures they read through a pool](../../../docs/reference/memory.md#shaders-report-which-textures-they-read-through-a-pool)
- [Repeated GPU out-of-memory failures end the game](../../../docs/reference/memory.md#repeated-gpu-out-of-memory-failures-end-the-game)
- [Texture IDs of one storage share one packaged texture](../../../docs/reference/nvn-objects.md#texture-ids-of-one-storage-share-one-packaged-texture)
- [Finalizing a copy of a texture object](../../../docs/reference/nvn-objects.md#finalizing-a-copy-of-a-texture-object)
- [Dynamic resolution that never moves, and draw passes that wait for each other](../../../docs/reference/frame-rate.md#dynamic-resolution-that-never-moves-and-draw-passes-that-wait-for-each-other)
- [A full table of running GPU writes ends passes for nothing](../../../docs/reference/frame-rate.md#a-full-table-of-running-gpu-writes-ends-passes-for-nothing)
- [Float atomics in storage memory](../../../docs/reference/shaders.md#float-atomics-in-storage-memory)
- [textureGatherOffsets and a loop over its offsets](../../../docs/reference/shaders.md#texturegatheroffsets-and-a-loop-over-its-offsets)
- [Image atomics on a 32-bit colour texture](../../../docs/reference/shaders.md#image-atomics-on-a-32-bit-colour-texture)
- [A host's own store in a fragment shader moves its depth test](../../../docs/reference/frame-rate.md#a-hosts-own-store-in-a-fragment-shader-moves-its-depth-test)
- [Dead-code removal dropped a coordinate of a multi-output instruction](../../../docs/reference/shaders.md#dead-code-removal-dropped-a-coordinate-of-a-multi-output-instruction)
