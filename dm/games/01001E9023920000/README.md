# Trails in the Sky 1st Chapter (`01001E9023920000`)

Update 1.0.6.1 (`v393216`) over the base game, SDK 19.3.13, NVN. Falcom's SORA engine, with FMOD sound and the SDK's movie player. Modules `main`, `subsdk0`
(NintendoSDK_movie) and `sdk`; `sdk` and `subsdk0` are the same SDK 19.3.13 builds as [Octopath Traveler 0](../01005270232F2000/README.md)'s. Started from the provisional
profile the NSP script wrote from its survey (_dm/games/profile_guess.py_). Plays on the Mac at 60 fps; the IPA (8.3 GB) has not been run on an iPhone yet. The copy at hand
is the Korean/Asian release: its data holds Korean and Simplified and Traditional Chinese text and no English or Japanese.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x1A8000000`, `mem_alias 0x100000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | virtual address memory (the NPDM declares 16 MB of system resource): the heap the game asks for is 0x81200000, and its engine reserves an address region at start-up (below), so the guest block is 6.625 GB: 4 GB of alias region, 0.25 GB of stack, 2.125 GB of heap and the 0.25 GB the modules start above |
| the virtual address reservation cut: `fmov d0,#10.0` → `fmov d0,#3.0` at `main+0x5746f4` | [`patches.py`](patches.py), `main` build `6656ba1b…` | the engine's native allocator reserves **ten times** its 0x48200000-byte capacity as a virtual address region, rounded to 2 MB: 12.1 GB, more than any alias region a phone can hold. The engine only maps what it uses (402 MB through the opening movie), so a factor of 3 (3.375 GB) is enough and fits the 4 GB alias region |
| `shader_groups` and `shader_note` | [`game.tsv`](game.tsv) | RomFS folder names the split groups the shader sources by (the Korean/Chinese/English-named asset folders) |

No SDK build of its own is patched: `sdk` and `subsdk0` are the SDK 19.3.13 builds Octopath Traveler 0 registered in `dm/tools/sdk_patches.py` (the AddressRegionManager node area cut among them).

## Files and shaders

- RomFS: 14.3 GB in 72,358 files: `asset/nx` (5.96 GB: models `.mdl`, textures `.xtx`, shaders `.nvn`, images), `asset/common` (3.67 GB), `voice/nxopus` and `se/nxopus` (Opus, 21,193 files), `scene` (1,135 `.json` / `.dat` scene files), `vfx`, `layout`, and the Korean/Chinese script and table folders (`script_kr`, `script_sc`, `script_tc`, the tables). Movies are in `movie_kr` / `movie_sc` / `movie_tc` (15 `.mp4`, played by `subsdk0`).
- Shaders: 6,600 programs as **GLSLC compiler outputs** (SDK 19 format, magic `0x19866891`) in `asset/nx/shader/*.nvn` (16,585 files, 6,582 unique programs: the same program ships per material) and 18 in `main`: 5,294 pixel, 1,269 vertex, 37 compute. When a control section's own length (+0x20) is 0x80 bytes shorter than its GPU code section's (`light_cull.nvn`), the split takes the pair from the output's section header ([GLSLC output whose control section is shorter than its code section](../../../docs/reference/shaders.md#glslc-output-whose-control-section-is-shorter-than-its-code-section)).
- The character G-buffer shaders write integer colour targets; the eye shaders (`chr_eye#*.nvn`) write one of them from float constants whose bit patterns are the integers, which the integer-output detection now reads ([Integer colour outputs written as float constants](../../../docs/reference/shaders.md#integer-colour-outputs-written-as-float-constants)). Without it the dialogue portraits' irises were missing (white eyes).
- `instance_cull.nvn`, one of the compute shaders, asks for 45,056 B of workgroup memory; the build moves it to a device-buffer slice per threadgroup ([Compute workgroup memory over 32 KB](../../../docs/reference/shaders.md#compute-workgroup-memory-over-32-kb)). Without it the dispatch did nothing and every instanced object in the scene was missing (the prologue's breakfast table, the chairs).

## Rendering

- The deferred character pass has a G-buffer of integer attachments (RGBA8Uint at colour 1-3 between float targets); compressed textures, MSAA resolves and the game's own post effects as in the other NVN titles.
- The 40-second opening plays through `subsdk0` (H.264 video, AAC audio) with sound; the title screen and the prologue's dialogue portraits are 3D, not video.
- Known open: four compute shaders ask for more than 32 KB of workgroup memory **and** use threadgroup barriers, so the rewrite above leaves them out; no dispatch of them came in the prologue. They would need the barrier flags replaced for device memory or another split.

## Performance and memory

- On the Mac, a 44-minute session from the title through the prologue and the guild's dialogues (157,214 frames): frame time p50 16.65 / p95 17.4 ms, GPU 6.0-7.4 ms a frame, CPU about half a core (5.0 mJ a frame, 59-60 fps), 2.97 GB resident, 2.4 GB of Metal allocations, no red line in the session's log.
- The prewarm list is empty (not recorded); the title and the prologue ran without it.

## Input

- Played with a controller on the Mac (the 3D pad layout); no profile key sets the layout. The game reads no touch in what was played.

## Mac route

- From the title: A at New Game, A at the difficulty choice, then A through the prologue's dialogue: the breakfast table at home, the walk, and the guild's shop counter (Rolent) where the dialogue portrait with the irises appears. `DM_CTL`'s `shot <path>` took the screenshots; the intro's cutscenes are long, so the route pressed A about every 1.3 s.

## Found on this game

- [A window's own branch onto its store](../../../docs/reference/rewriter.md#a-windows-own-branch-onto-its-store) and [An unconditional branch inside a window](../../../docs/reference/rewriter.md#an-unconditional-branch-inside-a-window): `main+0x2d6ca4`'s byte lock stopped the rewriter.
- [GLSLC output whose control section is shorter than its code section](../../../docs/reference/shaders.md#glslc-output-whose-control-section-is-shorter-than-its-code-section): `light_cull.nvn`'s key was not the host's.
- [Integer colour outputs written as float constants](../../../docs/reference/shaders.md#integer-colour-outputs-written-as-float-constants): the dialogue portraits' white eyes.
- [Compute workgroup memory over 32 KB](../../../docs/reference/shaders.md#compute-workgroup-memory-over-32-kb): the missing table.
- [NVN calls that were red stubs](../../../docs/reference/nvn-objects.md#nvn-calls-that-were-red-stubs): the ZCull compression switch, the control-memory callback switch and the texture-builder getters.

## Open

- iPhone: an IPA (8.3 GB) is built, not run on a device.
- The four compute shaders above.
- The prewarm list.
