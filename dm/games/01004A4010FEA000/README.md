# Bayonetta 3 (`01004A4010FEA000`)

Update 1.2.0 (v131072) over the base game, SDK 13.3.2 (no other game's `sdk` build). PlatinumGames' own engine on NVN; Wwise sound through `AudioOut`. Modules `main`, `subsdk0` (the movie library, NintendoSDK_movie: WebM movies, VP9) and `sdk`. Languages: English, Japanese, French, German, Spanish, Italian, Russian, Korean, Simplified and Traditional Chinese. Plays on the iPhone (IPA 14.2 GB); 60 fps in play, 30 where the game asks for it (the title and loading screens).

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0x107200000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource (META +0x14 = 0): no virtual address memory; `nninitStartup` sets a heap of the pool − used memory, which the game maps only as it touches it. The pool is a Switch's 3285 MB and 925 MB for the FrameBufferHeap of the [render scale](#render-scale): a heap of 4094 MB, kept under 4 GB. At scale 1.0 the game takes what it does on a Switch (96 MB of that pool stay free) |
| `mem_used 0x7400000` (116 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory (the game's `SetHeapSize` asks for pool − used) |
| `pad_layout full` | `game.tsv` | the 3D gamepad layout: the camera is on the right stick, and the D-pad picks items |
| Render scale stubs, 52 sites of main 1.2.0 | [`patches.py`](patches.py) | the game's sizes are immediates of this build; see [Render scale](#render-scale) |

## Services and SDK

- **Resolved: null read at `main+0xb8462c` right after the first two textures, called from `main+0x670eb0`.** Not the fault itself: `nnMain`'s start-up (`main+0x6705c0`) returns false when any of its 30 steps fails, the game then skips its main loop and goes straight to shutdown, and the shutdown walks a pool (`main+0x4efce70`) whose initialization (`main+0xb840d0`) came after the failed step. `DM_PROBE` on each step's entry (lldb was unavailable: developer mode off) showed the last one entered, `main+0x69c698`, the built-in shaders of `shader2.dat` (`hwfillcolor`, `hwshaderpf`, …), and probes inside it the step that failed: creating a shader from its object (`main+0xd0d998`). The engine checks each shader object's GPU code version (byte 6 major, byte 7 minor of the object) against the driver's range (`nvnDeviceGetInteger` 0x2E–0x31: newest major, oldest major, newest minor, oldest minor) and refuses newer code; the host reported 1.14, the table measured on Super Mario Odyssey's SDK 7 driver, and `hwfillcolor.vso` is 1.15 (the game's objects run from 1.9 to 1.16). The host now reports the translator's supported GPU-code range; see [Newest GPU code version reported by the driver](../../../docs/reference/nvn-objects.md#newest-gpu-code-version-reported-by-the-driver).
- Two NVN getters were red stubs: `nvnWindowGetCrop` (read back every few frames; before any `SetCrop` it is the whole first window texture) and `nvnSamplerBuilderGetLodClamp`.
- **Not a hang:** the first boot sat on the loading clock for about a minute, the CPU idle, with every file read done after 12 s: that run compiled its Metal libraries and pipelines on first use (cold cache). With a warm cache the title is up after about 15 s; what looked like a 40-second wait in the read log (`DM_FSLOG`) was the title screen itself, which reads nothing. Screenshots every few seconds (`ctl.sh snap` from a script) told the two apart.

## Rendering

- Shaders are single-stage programs: the engine makes a program of each object and binds them per stage at draw time, so each is paired with a cover pixel shader. It finds its built-in ones by name (`hwfillcolor.vso`, …).
- **Two domain shaders do not build** (`DS_Vanish05_*.dso`: `SPIRV-Cross threw an exception: Subgroup builtins are not available in this type of function`); the one-lane rewrite (`spv_single_lane`) covers vertex shaders only, and tessellated draws are skipped anyway.
- From the title to the prologue's first play the picture is right as far as compared (title scene, menus, the VP9 opening movie, the first real-time scene, play with the HUD). Two red lines, once each: a pixel shader declares a cube texture where the game binds a 2D one (the zero cube is bound), and a draw whose stage 0 reads cb1 with none bound.
- The spin-wait search takes 37 loops in `main` (35 before its store rule) ([development.md, Power](../../../docs/development.md#power)).

## Render scale

**Current: implemented and played at 1.0 and 1.2 on an M4 Max (2026-11-12, the prologue's first scenes: 1920x1080 swap chain, frame time p50 16.66 ms, GPU about 6 ms a frame against 5.5 ms at 1.0); 1.6 and 2.0 are fixed below and not played again yet.** `DM_RENDER_SCALE` 1.0 to 2.0 multiplies the docked game's window and scene: 1.2 gives a 1920x1080 window and a 1920x1080 scene (every row, not the 1072 of the older experiment), 1.6 2560x1440, 2.0 3200x1800. 1.0, the default, is the game as it is. Handheld mode and a scale below 1.0 keep the game's sizes.

How the engine sizes things (update 1.2.0, `main` `b47cc79b88b7091ea514b2b725c50ae0`):

- **Nothing is resized at run time.** At start-up the game makes each render target once at its largest size (a *backing*) and, for each of eight resolution levels, a *view* of every backing with that level's size; a view has no memory of its own. `main+0x69d344` makes the window-sized backings (1600x900: depth, half-size depth, five colour targets) and their views from the tables at `main+0x182d670` / `0x182d570` (docked) and `main+0x182d4d0` / `0x182d450` (handheld; both sets are made in either mode). `main+0x69de80` makes the scene-sized backings (1600x896 and its half, quarter ... chains, the bloom and exposure targets among them) and, through `main+0x6a05d8`, their views per level, from immediates. A second family of eight levels (docked 1280x720, 1280x720, 1120x628, 960x540, 800x448, 720x404, 640x360, 640x360) has three 1280x720 backings of its own (`main+0x69dd70`, `main+0x69de80`, views in `main+0x6a0fc8`). `main+0x69fda8` runs the scene's set-up again when the operation mode changes.
- **A view that needs more storage than its backing fails the start-up** (`main+0xd0f2f0`, the compare at `main+0xd0f4fc`), and the failed start-up then faults in the shutdown (`main+0xb8462c`). That is the crash of the older 23-site experiment: views of 1920x1072 over 1600x900 backings.
- **The backings come out of the FrameBufferHeap**, 260 MB (`main+0x69d164`; beside it the 42 MB ShaderCodeHeap). Each engine heap is a memory block of its own from the nn::os heap (`main+0xcd5714`: `nn::os::AllocateMemoryBlock`, whole 2 MB, then an NVN memory pool over it), so the FrameBufferHeap has no parent heap to grow with it: what bounds it is the application pool. On a Switch's pool the game's fixed blocks (`main+0x95db40`: 403, 111, 17, 41, 833 and 1348 MB; the two heaps; 1 MB; a 4 MB thread-local block) leave 96 MB free, which is why raising the heap alone to 512 MB failed its allocation in the older experiment, and why its 27-site form, with every level at the largest size, ran out of the 260 MB.
- **The window's size** is what `main+0x70a740` returns (1600, 900): the swap chain (`main+0xd0c990`) and the size the engine reports to its callers (`main+0xd02d90` / `0xd02db0`) come from there.

What the stubs do ([`patches.py`](patches.py), `FRAME_STEP` of this build, which gives the state page and the loader's `DM_RENDER_SCALE` at +0x10; the profile has no frame-rate target, so the host's pacing is unchanged):

- 52 sites: the first, after the operation mode is read (`main+0x69c6cc`), sets the scale to 1.0 in handheld mode and below 1.0; 47 run the site's own instruction and then multiply its value (widths to a multiple of 16, heights to a multiple of 8 with the factor 900/896, so the scene's largest level has exactly the window's rows; the window's height is that same value); one makes the FrameBufferHeap 293 MB times the scale squared, one gives the exposure's column buffer its entries and two the Forward+ light grid its capacity (below). A stub leaves the value alone when the scale's bits are those of 1.0.
- The sites were found, not listed by hand: each immediate of 16 to 8191 in the set-up functions and each table entry was changed alone and the set-up run in an interpreter with its callees stubbed (`dm/tools/a64emu.py`), which shows which arguments of which creation call each one reaches.
- Sizes at 1.2: window and scene 1920x1080, levels 1808x1024, 1728x984, 1632x928, 1536x872, 1232x696, 768x432, the second family 1536x872 down to 768x432, FrameBufferHeap 422 MB (1.6: 752 MB; 2.0: 1172 MB).
- **Not scaled, on purpose:** the shadow maps (below), the sets of fixed size that `main+0x69de80` makes (the 800x450 targets and their depth, 480x270, 640x360), the 1280x720 targets of `main+0x69d344` and the small squares. Each is made and used at one size whatever the scene's is; whether any of them is a visible part of the picture at 1080p is a question for the first played run.

Checked (no game run):

- `dm/tools/tests/test_bayonetta3_render_scale.py` (no game files): the policy, the profile's memory for the largest heap, and every stub's machine code run in the interpreter at 13 scales against the policy, with the other registers and the flags unchanged.
- On the game's `main` (the work folder's `enhanced-impl/tools/verify_scale.py`, with the stubs assembled over the original image, and `verify_rewritten.py`, on the rewriter's `main.patched.bin`): at 1.0, in handheld mode at any scale and below 1.0 the set-up makes the unpatched main's 708 creation calls, argument for argument (24 cases); at ten scales from 1.0001 to 2.0 every view fits its backing by its sizes wherever it does in the unpatched main, and the window, the backings, the levels and the heap are the policy's; started docked at 1.2 or 2.0 and switched to handheld, the handheld views keep the game's sizes and fit. The functions the sites are in use none of the stubs' registers at a site.

- **A white title screen and a flickering white picture at 1.6 and 2.0, with 1.2 right** (played 2026-11-12; the passes of a frame are the same at 1.2 and 1.6, by the host's `[gpuprof]` lines, so the fault is in what they read). Two things the game sizes once and fills by the size being drawn; **fixed in the code, not played again yet:**
  - **The Forward+ light grid.** The light lists are kept per tile of 16 pixels; each frame `main+0x686660` asks for (width + 15) / 16 by (height + 15) / 16 tiles of the size being drawn, with no upper bound, and `tilebasedfowardplusps` (a pixel shader with no colour target, drawn once a frame) writes each tile's lists into buffers that `main+0xd578b0` makes once for **120 by 68 tiles**: 1920x1088, which is why 1.2 fills them exactly and 1.6 (160 by 90) and 2.0 (200 by 113) write far past them. The stubs at `main+0xd57964` and `main+0xd57994` make the capacity the largest level's tiles and the two written-out buffer sizes 4 words a tile; the other two buffers already follow the stored capacity.
  - **The exposure's column buffer.** `averagebrightnessps1` draws one fragment for every two columns of the quarter-size scene and stores that pair's luminance sum at the fragment's x; `averagebrightnessps2` adds the entries up and writes the exposure the tone mapping reads. The buffer has 256 entries (`main+0xd5870c`: 4 bytes times 0x100 at `main+0xd587b0`): 240 are written at 1920 wide, 320 at 2560. The stub there scales the 256 like a width. Played with this fix alone at 2.0: the picture was still white, so it was not the cause seen, though the writes past the buffer are real by the code.
  - Both checked in the interpreter on the rewritten main (`verify_buffers.py`): at 1.0 and 1.2 the grid is the game's 120 by 68; at 1.35, 1.6, 1.9 and 2.0 it is what the largest level asks for, and the column buffer has more entries than are written.

**Open, for the played runs:** whether the picture is right at 1.2 (final composition, HUD and text positions, bloom and exposure, the fixed-size sets above, movies, menus), the frame times, and what the game's own level choice does on this GPU. Other things the game may size once for at most 1920x1088, as it did the light grid, are not ruled out. 3840x2160 (2.4) is not offered: its heap of 1688 MB does not fit the 5 GB layout. On the iPhone the larger pool may move the guest's memory into files (`guest_file_wanted` compares the pool with the room to the memory limit); not run there. A front end's render-scale row shows 0.5 to 2.0, of which 0.5 to 0.95 do nothing here, and no `render_base` is set because the handheld size is not scaled.

### Found for later settings (static, nothing changed)

- **Shadow maps:** one 2048x2048 colour backing and one depth (`main+0x69e7f4`, `main+0x69dda0`), with three views each: docked 2048, 1600 and 1024 squares (`main+0x69f348`, `0x69f3d0`, `0x69f450`), handheld 1792, 1472 and 896. A sharper setting scales the two backings and the three views together; the sampling reciprocals and the cascades' use of the three sizes are not traced yet.
- **Texture filtering:** samplers are made at `main+0xd11ed8`; the anisotropy is one of five levels (1, 2, 4, 8, 16: the table at `main+0x185c4d4`, read only at `main+0xd11fb0` by the index at +0xc of the sampler's description), and the host passes it to Metal as it is. The game never calls `nvnSamplerBuilderSetLodBias`. Which levels its materials ask for needs a run.
- **Anti-aliasing:** `FilterShaderFXAA.pso` (`c0faec08dc7005df`) is used in play (the earlier baseline route loaded it, and the prewarm list has it); `FilterShaderFXAA_g.pso` and the four SMAA shaders (`filtersmaaedge`, `filtersmaaweight`, `filtersmaablend`, `filtersmaacopy`) were not loaded on that route. No temporal pass was found.

## Files and shaders

- 72 PKZ archives in the RomFS root (14 GB; `root_000.pkz`, `core_000.pkz`, `pl_000.pkz`, `st1_000.pkz`, `sound_000.pkz` … `sound_017.pkz`, …) of DAT archives, compressed per entry with Oodle Kraken or stored (`OodleKraken` 11,760 entries, `None` 5,159; `main` knows `ZStandard` too, `main+0xcbdf80` maps the three names to 0xc9, 0x65 and 0), beside 478 loose WebM movies in `movie/` ([01](../../../docs/agents/01-extract.md#compressed-romfs-content)). `romfs_marker` is `root_000.pkz`.
- `main` reads the archives only through Oodle's streaming decoder (`main+0xcbc698` creates one, `main+0xcbc7f8` feeds it 256 KB at a time into a 10 MB window), so `OodleLZ_Decompress` was never linked in and its name string is not there. The Mac host's Oodle tool mode now finds `OodleLZDecoder_MemorySizeNeeded`, `Create`, `Destroy` and `DecodeSome` instead (by the strings of Oodle's `oodlelzcompressors.cpp` and their order) and decodes each entry with them in `DM_OODLE_SERVE` (`hle_oodle.c`). `profile_guess.py` turns it on for any game with Kraken PKZ archives (`oodle_packs`). The shader split decodes every entry of every PKZ (8 workers, one Mac host each) in about 20 s; only `root_000.pkz` holds shaders.
- Shaders: 8,024 PlatinumGames shader objects in `root_000.pkz` (`shader.dat`, `shader2.dat`, `shadereff*.dat`, `autoshadereff.dat`): `VSO\0` / `PSO\0` / `CSO\0` / `HSO\0` / `DSO\0` (the stage), u32 compiler version, u32 control length, u32 control offset (0x18), u32 code length, u32 code offset. 6,424 distinct keys (4,288 pixel, 2,047 vertex, 78 compute, 6 domain, 5 hull) (`shader_split.py` `pkz_entries`, `pgdat_members`, `pg_shader`). All but two domain shaders build.

## Status on the Mac

- The boot, the title, a new game (difficulty, Naive Angel Mode, brightness, the auto-save notice), the VP9 opening movie, the prologue's first real-time scene and a minute of play in it, with sound.
- 60 fps in play (frame time p50 16.67 ms, p95 17.5 ms; GPU about 5 ms a frame on an M6), 30 where the game asks for it (the title and loading screens); about 2 GB resident and 3.1 GB of Metal allocations, flat after the first scene. The title is up about 15 s after launch with a warm shader cache. The prewarm list covers that route (678 lines).

## Mac routes

- The work folder's `run/ctl.sh` drives a run started with `DM_CTL` (`CTL=<file>` picks another run's control file), `run/step.sh <shot> <presses…>` presses and takes a screenshot, `run/snapseq.sh` boots and takes one every few seconds, and `run/route.sh <run>` boots on a fresh save and drives the route above: A at the title, A through the logo, A (Standard), A, B (closes the Naive Angel Mode help), A (Off), A (brightness), A (the auto-save notice), then Plus, Down, A skips the opening movie.

## Found on this game

General host changes first needed here; each applies to every game:

- **The NVN layer advertises support for the game’s GPU-code versions** (`hle_nvn.c`), `nvnWindowGetCrop` and `nvnSamplerBuilderGetLodClamp`: [Newest GPU code version reported by the driver](../../../docs/reference/nvn-objects.md#newest-gpu-code-version-reported-by-the-driver), [NVN calls that were red stubs](../../../docs/reference/nvn-objects.md#nvn-calls-that-were-red-stubs).
- **Oodle's streaming decoder in the Mac host's tool mode** (`hle_oodle.c`, `OodleLZDecoder_*`, `oodle_packs`) and the **PKZ / DAT readers**: [01, Compressed RomFS content](../../../docs/agents/01-extract.md#compressed-romfs-content).
- **PlatinumGames shader objects** (`pg_shader`): [PlatinumGames' shader objects](../../../docs/reference/shaders.md#platinumgames-shader-objects).

## Open

- Dotted patches on some surfaces (a grid every 4 pixels: the platforms breaking up in the prologue, a floor near the camera) look like the engine's dithered dissolve; not compared with the Switch yet.
- The two domain shaders and the two red lines above.

## Enhanced Mac graphics investigation (2026-11-11, current)

**Superseded in part (2026-11-12):** the resolution findings below are resolved in [Render scale](#render-scale), which is the current state: the profile now has a
`patches.py` with render scale stubs, the FrameBufferHeap's parent is known (the application pool), and all eight levels and the second family are covered. The rest
of this section is the earlier investigation as it was written, kept for its evidence and its plan for the later settings.

**First deliverable: feasibility and implementation plan, not an enhanced build.**
The investigation checkout is `codex/bayonetta3-enhanced`, based on seedds/nxnative
`3553400d` from the requested `origin-dir/nxnative` checkout. Its shader chain is
`3ae08e02ec6a`. No host, translator, profile constants or launcher controls were
changed, and no shader compilation was run. The earlier Mac gameplay results above
remain their original measurements; this investigation did not measure gameplay
frame rates on the M4 Max.

### Inputs and evidence

The local base `Bayonetta 3 [01004A4010FEA000][v0].nsp` and update
`Bayonetta 3 [01004A4010FEA800][v131072].nsp` were found together in the user's
Downloads NSP folder. Existing extraction in MacNativeGames' `games/by3/BY3_build`
was reused for analysis, without opening keys, extracting firmware or changing
NSPs. Its `extracted/v120/exefs/main` has SHA-256
`499983f6adabc071488be7fe75f547c079c8c8dac95c3573e78a4277d7641800` and NSO build ID
`b47cc79b88b7091ea514b2b725c50ae01c6f00df` (the profile keys on its first 16 bytes).
The NSO loader verified the segment hashes. The existing merged RomFS is a
15,047,546,224-byte `romfs.bin` with its index; it is not nxnative's `DMROMFS1`
image and cannot be reused by renaming it. A checked conversion/repack or normal
NSP extraction is needed for the nxnative build.

The previous experiment survives in MacNativeGames' local checkout
`c2771821e9828f37ab7e68d664ba2779ef0885e6`, including:

- `games/by3/scripts/make_res_patch.py` and `BY3_build/patches/res-*.tsv`;
- `Bayonetta 3 PR1752.app`, `Bayonetta 3 HiRes Test.app`, the rewritten modules,
  executable and its debug symbols;
- `~/Library/Logs/MacNativeGames/by3.log` (23 sites) and `by3.previous.log`
  (a separate six-site test), both ending at `main+0xb8462c`.

New debugger logs and a machine-readable summary live beside the NSPs under
`01004A4010FEA000-work/enhanced-investigation/`; the future build has a separate
`01004A4010FEA000_enhanced_build/` directory. Debugger experiments used the old
executable and its matching rewritten modules, checked original bytes, and made
changes only in process memory. Every run was killed at a resource-setup
breakpoint before built-in shader loading. GPU submission was disabled in this
old host configuration. These are CPU resource-setup tests, not Metal rendering
or gameplay validation. Installed applications and player saves were not used.

The old runtime has about 43 MiB of cache under
`build/shader-cache/rt-totk-native-v9-unverified`. It is not a compatible nxnative
`b-3ae08e02ec6a` library. No matching nxnative Bayonetta cache was found in the
surveyed game/Downloads locations. Do not relabel the old cache to bypass chain,
reflection or profile-input checks. Existing source shader containers and the
extracted game files can still save extraction work.

### Native sizes and dynamic resolution

Disassembly distinguishes the scene from the presented frame. Docked scene
initialization selects **1600x896**, handheld **1280x720**
(`main+0x69c6d0..0x69c6e4`); the stock NVN window was logged as **1600x900**.
Thus the old launcher's "900p" label does not establish a true 1600x900 scene.
The active viewport, crop and final composition still need a live pass capture
in the current nxnative host.

There is a dynamic-resolution resource family. Its width table at
`main+0x182d670` and height table at `main+0x182d570` contain:

| Index | Width x height |
|---|---|
| 0, 1 | 1600x896 |
| 2 | 1504x848 |
| 3 | 1440x816 |
| 4 | 1360x768 |
| 5 | 1280x720 |
| 6 | 1024x576 |
| 7 | 640x360 |

`main+0x69d6bc..0x69d838` builds views using **eight** table entries. The old
patch generator changes only seven. Code at `main+0x69f278` also explicitly
constructs index 7 at 640x360; additional size families/fallbacks remain elsewhere.
These prove that the engine supports multiple internal sizes. Which levels it
selects under each scene/load, and its control thresholds, have not been measured
here. Do not describe the old seven-entry patch as a complete DRS disable.

The old "1080p" generator rounds height down to a multiple of 16: it requests
**1920x1072**, not 1920x1080. Its 1440p request is 2560x1440. A true 1080p feature
must rasterize 1080 active rows and carry matching depth, sampling and composition
constants; padded storage, if required, must not reduce that active rectangle.

### Why the previous high-resolution test crashed

**Resolved for the retained 23-site test:** the SIGSEGV is a secondary shutdown
fault, not the original failed allocation. The log's main base and PC identify
`main+0xb8462c`, whose instruction is `ldr x26, [x8, #8]`, with `x8 == 0` and
fault address 8. Its caller is shutdown at `main+0x670eac`. The known shader-code
version problem above can reach the same shutdown, but the new debugger trace
identifies a different startup failure for this experiment.

The 23-site table is the 27-site table **without** `0x69c720`, `0x69c728`,
`0x69c73c` and `0x69c740`. It enlarges resolution-dependent views while leaving
backing resources smaller. The first failing view comes from
`main+0x69d76c` -> `0xcfa914` -> `0xcfe598` -> `0xd0f2f0`:

- Requested descriptor: **1920x1072**.
- Required NVN storage: **0x870000 = 8.4375 MiB**, in `x24`.
- Existing backing texture storage: **0x640000 = 6.25 MiB**, returned in `w0`.
- `main+0xd0f4fc` compares them; `0xd0f500` branches to failure because the view
  needs more storage than the backing texture provides.
- `0x69d344` then returns false to `0x69c730`. Startup abandons initialization;
  shutdown later walks an uninitialized pool and faults.

This is a concrete resource-size invariant violation, not evidence of insufficient
48 GB system RAM or a MetalFX failure. Removing more patches or skipping that
comparison would hide the cause.

**Resolved for the full 27-site setup failure:** applying all original sites in
memory gets past that first mismatch, then the texture requested by
`main+0x69ea3c` fails. Its descriptor is **960x536**. At `main+0xd0f574`,
`cd39bc` is asked for **0x4b0000 = 4.6875 MiB**, aligned to 4096 bytes, from the
framebuffer allocator; it returns null at `0xd0f578`. The enclosing call chain
returns false through `0xcfe49c` and `0x69ea40`.

The allocator is not an unnamed suspected heap anymore:
`main+0x69d164` supplies **0x10400000 = 260 MiB** for configuration field +0x90;
`0xd0c844..0xd0c858` passes it to heap creation at renderer +0x28, alongside the
literal name **FrameBufferHeap**. Field +0x98 is a separate **42 MiB
ShaderCodeHeap** (`0xd0c860..0xd0c874`). The failed texture allocates from
renderer +0x28. The unchanged framebuffer budget cannot satisfy the enlarged
resource set at that point. The exact free-list/fragmentation contribution and
its parent budget remain open; do not derive a shipping heap size solely from
pixel ratios.

Two additional diagnostic results prevent unsafe reuse:

| Configuration | Observed resource-setup result |
|---|---|
| Stock, no resolution patch | Reaches `main+0x69c754`, after render setup, with success |
| Old 23-site 1080p | Backing-storage comparison fails as above |
| Original full 27-site 1080p | FrameBufferHeap allocation returns null as above |
| Original full fixed-900p table | Later view needs `0x578000` bytes but backing has only `0x3c0000`; fails the same storage comparison |
| Full 27-site 1080p, only FrameBufferHeap raised to 512 MiB | Earlier graphics initialization fails, returning through `0x69c704`; no render-setup success |

The last test changes only `main+0x69d164`, checked original `0x52a2080a` to
`0x52a4000a`, in debugger memory. It is a diagnostic counterexample, not a
recommended heap patch. Child heap growth must fit its parent allocator, NVN
pool and guest address layout. The fixed-900p failure also means the historical
MacNativeGames claim that its generated fixed-900p table works does not validate
these retained artifacts. Keep the stock DRS path as the initial baseline.
The full 27-site run still logged a 1600x900 NVN window: its alleged "window"
patches do not establish a larger swapchain either.

### Implementation points and architectural references

| Concern | Exact place to start | Required invariant |
|---|---|---|
| Build identity / patch application | Add this profile's `patches.py`; `dm/tools/patch_nso.py` `CUTS`, `STUBS`, `CAPABILITIES` | Only build `b47cc79b88b7091ea514b2b725c50ae0`; assert original bytes; fail the whole option on mismatch |
| Mode-selected scene dimensions | `main+0x69c6d0..0x69c6e4` | Logical scene, active viewport and mode policy agree |
| Screen-size backing resources | Arguments at `0x69c720/728`, setup `0x69d344`; largest scene at `0x69c73c/740`, setup `0x69de80` | Every view fits its own backing allocation; separately trace the actual swapchain |
| DRS resources and sampling | Tables `0x182d570/670`, view loop `0x69d6bc`, families at `0x69f0b8` onward; `0x6a05d8` | Include all eight entries and exceptional families; dimensions, UVs, copies and dispatch bounds agree |
| GPU-memory budget | `0x69d164`, `0xd0c844..0xd0c858`, allocator call `0xd0f574`; profile `app_pool` / `mem_reserve` / alias / stack | Grow the framebuffer child, parent and pool coherently; fit the guest address layout |
| Presentation / MetalFX | `dm/gfx/nvn/nvn_mtl.m` `fx_wanted`, `fx_encode`, `present_viewport`; `DM_PRESENT_UPSCALE` | This is spatial presentation upscaling, including UI; it is independent of internal rendering size |
| Launcher | `dm/games/settings.tsv`, `settings.py`; `dm/mac/Launcher.swift`, shared `LauncherSettings.swift` / `LauncherRow.swift`, `launcher.c` | Gate each new choice on a verified capability and apply dependent settings atomically |
| Timing | `dm/host/hle_fpslock.c`, `hle_nvn.c`; profile timing metadata | Change the engine clock/limiter together with presentation; never raise display Hz alone |
| Packaging | `scripts/build_ipa_from_nsp.py`, `dm/mac/pack_app.py` | Embed RomFS, rewritten modules, matching libraries, tables, fonts and host; no external work-folder dependency |

The useful Zelda reference is coordinated scene/output/allocator growth, not its
addresses or its multiplier. MacNativeGames' inspected
[`games/totk/runtime/game_patches.c`](https://github.com/ly05010419/MacNativeGames/blob/main/games/totk/runtime/game_patches.c)
(`runtime_render_res`, `runtime_output_res_extra_bytes`) grows
DynamicTextureAllocatorMem, its Graphics parent and the application pool together.
Its scene/output separation and its verified shadow option
(`games/totk/runtime/botw_shadow_res.h` in the local snapshot) are good patterns.
Bayonetta uses PlatinumGames' heaps, not Zelda's agl HeapDef or cascade layout;
none of Zelda's sites or sizing formulas is transferable without measurement.

The current Bayonetta profile has no `patches.py`, no enabled FPS target, and no
verified render-scale/DRS capability. The existing Mac settings expose console
mode, Standard/MetalFX upscaling, fullscreen, Metal 4 pipelines, pipeline prebuild,
encode thread, frame buffering and performance HUD, plus language/input choices.
They do not currently offer a functional Bayonetta internal-resolution, shadow,
AA or frame-rate selector. The generic FPS-target finder was run on this exact
main and reported **no target frame rate**; there is no Bayonetta-specific row
in `hle_fpslock.c`. Setting a generic environment variable is insufficient.

### Prioritized implementation and acceptance gates

1. **Stock / 900p-class baseline first.** Reuse verified extraction through a
   checked repack, establish a current nxnative Mac run in isolated save/cache
   directories, and capture title, prologue gameplay, movie and menu passes.
   Measure scene/depth targets, viewport, crop, post chain, UI surface and GPU
   time with `DM_NVN_PASSDUMP` and `DM_NVN_GPUPROF`. Compare Standard with
   `DM_PRESENT_UPSCALE=metalfx` at the same internal size. Initially describe the
   native docked scene honestly as 1600x896 with a 1600x900 frame. A strict
   1600x900 scene or a fixed maximum requires the same resource audit as 1080p;
   do not ship the old fixed-900p table.
2. **True 1920x1080, as the first enlarged-rendering experiment.** Inventory each
   backing texture and its views, lifetimes and framebuffer-heap consumption.
   Trace the parent budget before changing the 260 MiB child. Preserve a single
   consistent maximum and let lower DRS levels retain their semantics, rather
   than blindly expanding every level. Provide 1080 active rows, matching depth,
   post-processing constants and a verified final composition. Require clean
   allocation logs, Metal validation and scene/UI captures before a launcher
   option appears. A 1920x1072 setup test is not this milestone.
3. **True 2560x1440**, using the proven sizing path, then **optional 3840x2160**
   only if measured memory and frame-time margins allow it. Relative to
   1600x900 these contain 2.56x and 5.76x as many pixels (1080p: 1.44x); this
   predicts neither FPS nor whole-game memory. Test one size at a time.
4. **AA after resolution.** Shader inventory contains `FilterShaderFXAA.pso`
   (`c0faec08dc7005df`) and `_g` (`ff76d234e2809d50`). Capture whether they execute,
   their constants and their placement relative to tone mapping and UI. Compare
   a verified FXAA tuning/bypass against native higher-resolution edges in
   motion. Shader names alone do not prove a pass is active. The old pass study
   reports no TAA/jitter/velocity buffer; do not promise temporal MetalFX without
   finding the required inputs. Avoid stacking another blurry filter by default.
5. **Shadows after allocation correctness.** Trace `ShadowCaster_*` and
   `PS_CnsShadow00_*` draws to depth targets and cascade/atlas consumers. Larger
   maps also require viewports, atlas offsets, sampling reciprocals, bias and
   budget updates. Existing 2048-square resources alone do not identify a safe
   quality switch. Expose no shadow selector until this is validated.
6. **60 FPS timing validation throughout every stage.** Target 16.67 ms
   gameplay frames, preserve native 30 FPS loading/title/movie behavior where
   requested, and compare wall-clock progression, animation/combat, audio and
   movies against stock. Record p50/p95/p99 CPU/GPU time and sustained busy-scene
   runs on this M4 Max. The prior Mac note's M6 figures are not an M4 benchmark.
   Only add a limiter/timing patch if measurement shows one is necessary.
7. **120 FPS last.** Find simulation step, animation, physics, input and movie
   clocks first. The old launcher's 120 Hz option has no Bayonetta frame-step
   implementation; its own runtime comment warns that raising Hz can double
   frame-dependent game speed. It is not proof of correct 120 FPS gameplay.

Presets are a later, measured mapping: Performance = verified native-size path
with optional MetalFX; Quality = verified true 1080p; Ultra = verified true 1440p.
All retain correct 60 FPS gameplay timing as the target. These are proposed
mappings, not available presets or guaranteed performance. Show a preset only
when its constituent options passed; keep 4K separately optional. Capabilities
must be separate from `FRAME_STEP`: using frame-step metadata just to expose a
resolution control would incorrectly change host pacing. Add resolution state
only as narrowly as the verified game patch needs.

Automated coverage for implementation should check all-or-nothing byte/build
matching, every DRS/view/backing relation including index 7, exact active sizes,
heap-budget overflow/alignment, capability-gated launcher choices and preset
mapping, and an isolated-save smoke route. Add assertions at NVN resource creation
for diagnostic runs. Validate colour and depth attachment sizes, compute dispatch
edges, bloom/exposure, HUD/text, pause/chapter menus, real-time cutscenes, VP9
movies and 16:9 composition. A changed screen-sized screenshot alone is not a
resolution test. Host/tool changes also need the regression selection and report
specified by [06-dev-loop](../../../docs/agents/06-dev-loop.md).

### Reproducible build and delivery route (not run in this investigation)

The existing packer already produces a self-contained Mac app. After graphics
validation, use this command from the experimental checkout, with the shared tool
venv's Python. It intentionally stages beside the NSPs:

```sh
export DM_GAME=01004A4010FEA000
B3_NSP="$HOME/Downloads/Nintendo Switch 1:2 — images/Bayonetta 3 [NSP]"
B3_KEYS="$HOME/Downloads/Nintendo Switch 1:2 — images/prod.keys"
B3_BUILD="$B3_NSP/01004A4010FEA000_enhanced_build"
B3_STAGE="$B3_NSP/Bayonetta 3 Enhanced Candidate.app"
python3 scripts/build_ipa_from_nsp.py "$B3_NSP" -k "$B3_KEYS" \
  --title-id "$DM_GAME" --package v131072 --platform mac-app \
  --work-dir "$B3_BUILD" --out "$B3_STAGE" \
  --bundle-id org.nxnative.bayonetta3.enhanced.candidate --mac-sign -
codesign --verify --deep --strict --verbose=2 "$B3_STAGE"
otool -L "$B3_STAGE/Contents/MacOS/dmhost"
```

This is a future baseline packing command, not an enhanced-build recipe until
this plan's code exists. A repeat build reuses compatible inputs; a first build
may translate all shaders. Do not run it during the investigation. The checked
Xcode 27.2 / Metal compiler matches `dmsh_chain.lock`. No valid local signing
identity was listed: `--mac-sign -` gives a locally usable **ad-hoc signature**;
Developer ID signing requires an installed identity passed instead. The existing
packer signs the inner host then the bundle, without hardened runtime. The host
requires executable guest mappings; notarized distribution is not provided by
this packaging route. See [Mac app](../../../docs/development.md#a-mac-app).

Test the candidate with `DM_SAVE`, `DM_ROOT` and `DM_TEMP` redirected under the
NSP work folder, leaving the real user save untouched. Check the embedded
libraries' chain and input stamp, dependent libraries, absence of external
symlinks/work-folder paths, bundle contents and signature. Test a copy launched
from another folder with the development paths unavailable. Record commit,
profile, shader-chain/input hashes and the options actually applied in its log.
Do not use `--seen-shaders` for the final complete library set. After acceptance,
repack as `Bayonetta 3.app` with the intended stable bundle identity and recheck
the signature. Only then copy to `/Applications/Games/Bayonetta 3.app`, preserving
any incumbent bundle and saves; no installation or replacement was done here.
