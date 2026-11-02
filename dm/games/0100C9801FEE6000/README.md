# Yakuza Kiwami (`0100C9801FEE6000`)

Update 1.01 (v65536) over the base game (v0), SDK 18.3.2. Ryu Ga Gotoku Studio's engine with CRIWARE, on NVN. Modules `main`, `subsdk0` (the movie module) and `sdk`, and one NRO the game loads with `nn::ro`, `data/module/nnx/lexus_minigame_nnx_retail.nro` (registered by `lexus_minigame_nnx_retail.nrr`), which the NSP script collects and rewrites beside them. Brought up on 2026-11-02 and played on the Mac (M3 Pro) and an iPhone19,4 (A20 Pro, 12 GB, iOS 27.2): boot, the title, the story with its cutscenes and fights. No game-specific rewriter cut or host change: the two general fixes it needed are under [Found on this game](#found-on-this-game). The README gallery picture is a Mac frame of a fight (2026-11-02).

Build ID of 1.01's `main` (first 16 hex digits, as cheat files name them): `AE90FD64E7B2FE1E`. Only 1.01 was brought up.

## What it needs

| Setting | Where | Why |
|---|---|---|
| one 36.62 GB guest block with a 36 GB alias region (`mem_reserve 0x928000000`, `mem_alias 0x900000000`) | [`game.tsv`](game.tsv) | the game's own address regions take 13 GB: see [Memory](#memory) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game selects near = 0 (one `nvnDeviceSetDepthMode` call at startup, in the Mac and the iPhone runs); with the default the Mac prebuild's pipeline bases are for the `main0` vertex entry and go unused, as the game's shaders run `main0_z`. No shader input, so no shader is built again |
| the profile's other keys | [`game.tsv`](game.tsv) | the provisional profile's static guesses (`build_ipa_from_nsp.py` / `profile_guess.py`), unchanged: they boot and play |
| ADR-relative jump tables whose bounds check is far before the branch | `dm/tools/patch_nso.py` | without it the rewrite of `main` stops at `main+0x91b24c`: [rewriter reference](../../../docs/reference/rewriter.md#adr-relative-jump-table-whose-bounds-check-is-scheduled-far-before-its-branch) |
| PARC archives and SLLZ streams | `dm/gfx/tools/shader_split.py` | without it the split finds no shader: [shaders reference](../../../docs/reference/shaders.md#glslc-outputs-in-ryu-ga-gotokus-parc-archives-and-sllz-streams) |

`dm/tools/sdk_patches.py`'s SDK-wide rules cut its `sdk` build (`f118ae1c2e7728471aaf1a0d58484968`, SDK 18.3.2, the build Absolum ships): the AddressRegionManager node area from 1 GB to 62 MB, and 2 MB alignment for regions of 4 MB and more, as SDK 20. All the runs below but one had an older form of those cuts, regions of 1 GB and more aligned to 32 MB (a per-build row the SDK-wide rule has replaced); the one with this profile's rewrite reached +33.65 GB of the alias region where the older cuts reached +33.75 GB: the rule only takes away alignment slack in front of the game's regions.

`shader_groups` and `shader_note` are as the provisional profile wrote them (the note still says so): every `shader_*` key is in each shader's cache key, and these are the ones the game was played with.

## Memory

- The NPDM declares 64 MB of system resource (virtual address memory), and `main` never sets a heap: the game maps all its memory itself, in address regions.
- It reserves one 32 GB region: the size is its configuration's `+0x28` (read at `main+0x379938`); its region setup (`main+0x6b5f0`) rounds it to 1 GB and takes it with `AllocateAddressRegion`, and a first-fit allocator of its own (`main+0x61c9c`, `main+0x61d80`) hands it out in 64 KB pages.
- From it the game's allocators reserve 13.02 GB: 256 KB read-only; a 4 GB growable heap (`main+0x54eb0`, 4 × the configuration's `+0x18`, which commits 1 GB); four pools (`main+0xbe4e0`, from `main+0xbc590`) of 1, 4, 2 and 2 GB plus 2 MB each, whose lower half goes out in 2 MB chunks and whose upper half is a heap from the midpoint and from the top down; and a 16 MB block. The 532 MB region it commits whole comes from `main+0x4e020`. About 1.5 GB is used at the title.
- The layout holds all of it unchanged: 256 MB, the 36 GB alias region, 256 MB of stacks and a 128 MB heap region the game never uses, one 36.62 GB block. The NSP script's address region check (the first 15 s in `nnMain`) saw the regions reach +33.72 GB of the alias region; the Mac run's highest mapped page was at +33.75 GB and the iPhone run's at +33.72 GB, with at most 2.25 GB mapped in either (the profile reports a 3.21 GB application pool).
- An iPhone19,4 on iOS 27.2 reserves that block: with the Increased Memory Limit entitlement the builds carry by default, its largest single reservation is 64 GB (at `0x7000000000`, where the run put the guest block), with or without extended-virtual-addressing; with neither entitlement, 6 GB ([memory reference](../../../docs/reference/memory.md#partitioning-the-guest-block-between-alias-region-stacks-and-heap)).
- A phone whose largest reservation is about 6 GB cannot hold the 13 GB of reservations in the 5 GB layout the provisional profile guessed (`mem_reserve 0x140000000`, `mem_alias 0x118000000`): the game's own sizes would need rewriter cuts ([Open](#open)).
- Peak memory: on the Mac 2.54 GB resident and 1.39 GB of GPU allocations; on the iPhone 2.04 GB resident and 1.36 GB of GPU allocations, with 6.15 GB still below the system's memory limit at the lowest (7.44 GB at the start). The long iPhone session below peaked at 2.24 GB resident (frame 35,400) and 1.34 GB of GPU allocations, with 5.95 GB still below the limit at the lowest (7.49 GB at the start); its regions' highest mapped page stayed at +33.72 GB, with 2.25 GB mapped at most.

## Shaders

- 2,462 shaders (1,259 vertex, 1,193 pixel, 9 geometry, 1 compute). 2,399 are in the four archives under `data/shader` and 63 more in `main`'s SLLZ streams, among them the only geometry and compute shaders; the map's source column credits `main` with 64, as one vertex shader is in both. How they are read: [shaders reference](../../../docs/reference/shaders.md#glslc-outputs-in-ryu-ga-gotokus-parc-archives-and-sllz-streams).
- They are single-stage GLSLC outputs: the split pairs the 1,259 vertex shaders with a cover of one pixel shader (`087ac2004509bba6`).
- The split prints three red lines for the sound CPKs (`data/soundcpk/bgm.cpk`, `data/soundcpk/se.cpk`, `data/stream/se.cpk`), which have an ITOC and no TOC, and scans each as one file. They hold the game's audio, no shader.
- No prewarm list yet (`pso_manifest.tsv` is empty): on the Mac the NSP script's pipeline prebuild took 31 s, and the first use of a pipeline costs a long frame (273 in the Mac run below).

## Frame rate

- The game asks for present interval 1 and paces itself: most frames take 33.3 ms (30 fps), and some stretches run at 60 fps.
- Mac (M3 Pro, window), 24,600 frames: the median 300-frame window has frame time p50 33.31 ms and p95 35.06 ms; 13 of the 82 windows ran at 60 fps (p50 16.7 ms); the longest frame took 1.74 s.
- iPhone19,4, 4,500 frames: the median window has p50 32.77 ms and p95 35.22 ms; 6 of the 15 windows ran at 60 fps; the longest frame took 1.06 s; thermal state 0 throughout.
- Neither run printed a `[RED]` line, an unimplemented call or a shader key missing from the map, and neither crashed.
- Mac, this profile's own rewrite (2026-11-02), 8,400 frames from a save into street fights: the median window p50 33.32 ms, 3 of 28 windows at 60 fps; peak 2.40 GB resident and 1.38 GB of GPU allocations; no crash, no unimplemented call, no shader key missing from the map. One red line, once, at frame 2614 as a new pipeline was made: `texture type mismatch: shader d303b2b1a0f401f7_b (stage 1) declares slot 134 as Metal type 5, texture ID 6493 is type 7 (NVN target 2, format 0x9a, Metal format 80); bound as is`, a 3D texture in a cube slot ([Open](#open)).
- iPhone Duo (A20 Pro), a long session (2026-11-04) on the build of the first iPhone run (main of 2026-11-02 with #627, #683 and #812 merged, so the older `sdk` cuts): 40,199 frames of play, about 22 minutes, over 73 minutes of wall clock. The median 300-frame window has frame time p50 33.31 ms and p95 35.05 ms (the worst window's p95 41.91 ms); 129 of the 134 windows ran at 30 fps and 5 at 60. The GPU took 7.82 ms a frame at the median window's p50 (the worst window's p95 11.92 ms) of the 33.3 ms budget; the CPU about three quarters of one core, nearly all of it on the efficiency cores (2% on the performance cores at the median window), at a median CPU energy of 0.10 W; thermal state 0 in all 268 samples. The phone's own prewarm list (`Documents/pso_user.tsv`, 1,300 lines recorded by earlier plays) made 1,276 shaders and 24 pipelines ready in 58 ms: past boot (432 ms at frame 3) the longest frame of play took 185 ms, where the first iPhone run's took 1.06 s. Twice the game went to the background for about 25 minutes (1,537 and 1,533 s) and resumed where it was, its audio session resuming without an interruption-ended notice; 621 of the session's 698 audio underruns (3.1 s of silence padding) fall in the one report that spans the first of them, and the other 77 add up to 166 ms. Folding and unfolding near the end switched the Duo layout live between the cover screen's split (the picture above the controls) and the inner screen's overlay. No `[RED]` line, no unimplemented call, no shader key missing from the map, no crash.

## Mac routes

- `scripts/build_ipa_from_nsp.py --platform mac` on the NSP folder, then the command it prints. The first start builds its pipelines before the first frame, so a stall limit (`DM_WATCH_STALL`) needs minutes, not seconds: 15 s ended a first run.
- The game suspends while its window is out of focus (`oe::SetFocusHandlingMode 3`): keep the window in front, with scripted input too.
- The NSP script knows an update by the `[v65536]` tag in its file name; a single update without one is taken for the update the profile registers (1.01).

## Found on this game

General changes first needed here; each applies to every game:

- **ADR-relative jump tables whose bounds check is far before the branch** (`jump_adr_far_check`, `dm/tools/patch_nso.py`): [rewriter reference](../../../docs/reference/rewriter.md#adr-relative-jump-table-whose-bounds-check-is-scheduled-far-before-its-branch).
- **GLSLC outputs in Ryu Ga Gotoku's PARC archives and SLLZ streams** (`par_entries`, `par_shaders`, `sllz_main_objects`, `dm/gfx/tools/shader_split.py`): [shaders reference](../../../docs/reference/shaders.md#glslc-outputs-in-ryu-ga-gotokus-parc-archives-and-sllz-streams).
- **An iPhone's largest reservation with Increased Memory Limit** (an iPhone19,4 on iOS 27.2: 64 GB; 6 GB without it): [memory reference](../../../docs/reference/memory.md#partitioning-the-guest-block-between-alias-region-stacks-and-heap).

## Open

- The 3D texture bound in shader `d303b2b1a0f401f7_b`'s cube slot 134 (one red line in a fight): which pass it is, and whether the slot is read; the symptom index has the 2D-in-cube and 2D-in-3D cases, not this one. Nothing wrong was seen on screen.
- A phone whose largest single reservation is about 6 GB: cut the game's 32 GB region and its pools to fit the 5 GB layout (the size read at `main+0x379938`, the 4 GB heap at `main+0x54eb0`, the pools at `main+0xbe4e0`). Not done: the iPhone19,4 needs none.
- A prewarm list in the profile (`pso_manifest.tsv`, empty): the iPhone has recorded one of its own (`Documents/pso_user.tsv`, 1,300 lines), with which the longest frame of a 22-minute session took 185 ms; a list recorded along a route (`DM_PSO_RECORD`) would give every player that from the first start.
- The 9 geometry shaders are translated; no run checked whether a scene draws with them.
- Other versions: only 1.01 (v65536) is registered; the base game (v0) was not run.
- The IPA's packaged size is not recorded, nor the Mac app's.
