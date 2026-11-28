# Digimon Story Time Stranger (`010062E01FE0C000`)

Update 1.2.2 (`v262144`), SDK 20.5.21. Media.Vision's MVGL engine on **NVN**, with CRIWARE audio/movies (`subsdk0` is `NintendoSDK_movie`). Modules `main`, `subsdk0`, and `sdk`.

Tested on Mac (Apple M6, macOS 27.2.1, Xcode 27.2) in both docked (`DM_OPMODE=1`) and handheld (`DM_OPMODE=0`) modes. Before the 60 fps change, the game was playable through the logos, autosave warning, privacy policy, DLC popups, opening 3D cutscene, protagonist selection, and early story gameplay (an 18.5-minute / 33,311-frame interactive docked session with sound, saving cleanly to slot `0000`, at 30.0 fps, GPU p50 6.05–7.25 ms, peak resident 2,795 MB / GPU 1,386 MB / alias mapped 1.75 GB, 0 `[RED]` lines, 0 missing shaders, 0 audio underruns; handheld mode verified at 30.0 fps, GPU p50 4.76 ms, 0 `[RED]` lines). iOS device validation is noted below.

## Frame rate

- The Switch code selects 30 fps twice at startup (1.2.2 `main+0x7d3c88` and `+0x7d41a8`). The MVGL setter (`+0xb95dcc` → `+0xbb4e50`) turns that into a 33,333 µs CPU budget. The main loop (`+0xbb4b80`) sleeps for the part of the budget it has left. NVN already presents at interval 1. The game steps its simulation by elapsed time (`+0xb20d40` → `+0xb11378`).
- [The rewriter](patches.py) selects 60 fps through the same setter, giving a 16,667 µs budget. It matches both startup functions together using reviewed fingerprints from the base game and 1.2.2. Other revisions get the change when both functions match. Cyber Sleuth 1.0.3 has different startup callers and limiter code, so the rule belongs to Time Stranger's profile.
- On an Apple M6 Mac in docked mode, 1.2.2 reached 59.6–60.0 fps in steady segments through the opening and protagonist selection. The same host and shader libraries ran the original executable at 30.0 fps in a steady title segment. The sampled simulation step fell from 33.34 ms to 16.68 ms. Bringing the window back to the foreground restored 59.8 fps after display-link callbacks stalled in the background, with the game's target still at 60. First-use Metal pipeline builds caused stalls in the next scene, which returned to 59.8 fps when the submit thread finished compiling. The run logged brief audio underruns, with no confirmed link to the new target. Base-game validation covered the rewrite. Handheld mode and iOS were not tested at 60 fps.

## What it needs

- **Memory layout (`stdalloc_cap 0xc0000000`):** the NPDM declares 20 MB of system resource (`sys_resource 0x1400000`) and `main` initializes `nn::mem::StandardAllocator` for 30 GB (`main+0x4e2360`). The provisional heap check's 1 GB cap (`0x40000000`) ran out of memory at frame 678 during title load (`CRASH sig=11 addr=0`, `lr=main+0xbc0bdc` after a failed 2 MB allocation). Setting `stdalloc_cap 0xc0000000` (3 GB) fits within the 4.375 GB alias region alongside the game's other address regions (~0.10 GB at boot) and held up through 18.5 minutes of gameplay (alias region mapped up to 1.75 GB).
- **Depth mode (`depth_mode zero`):** `main` calls `nvnDeviceSetDepthMode` with `near = 0`.
- **Wide MVGL archives and `NXVS`/`NXFS`/`NXCS` shaders:** all 51 RomFS `.mvgl` archives use Media.Vision's wide 64-bit/LZ4 `MDB1` layout (`dm/gfx/tools/mvgl.py`), holding 75,770 `.shad` entries (`37,885 NXVS` + `37,885 NXFS` → 19,078 unique RomFS shaders). Another 202 shaders (`28 NXVS`, `172 NXFS`, `2 NXCS`) are embedded as `NXVS`/`NXFS`/`NXCS` containers directly in `main` (19,280 unique base+update shaders; 22,937 with the 14 DLC packages).
- **Handheld MSAA resolve:** handheld mode (`DM_OPMODE=0`) resolves a `1920x1080` 4x `RG11B10Float` target into a `1280x720` 1x target via `nvnCommandBufferDownsample`; the host resolves into a cached private staging texture of the source size and blits the `1280x720` `(0, 0)` corner unscaled ([Multisample `Downsample` between different sizes](../../../docs/reference/rendering.md#multisample-downsample-between-different-sizes)).

## Found on this game

- [Media.Vision wide MVGL archives and `NXVS` / `NXFS` / `NXCS` shader containers](../../../docs/reference/shaders.md#mediavision-wide-mvgl-archives-and-nxvs--nxfs--nxcs-shader-containers)
- [Multisample `Downsample` between different sizes](../../../docs/reference/rendering.md#multisample-downsample-between-different-sizes)
- [Sampler builder queries leave renderer state unread](../../../docs/reference/nvn-objects.md#sampler-builder-queries-leave-renderer-state-unread) (reported in [#872](https://github.com/seedds/nxnative/issues/872) for v1.2.1)

## Open

- **Intermittent iPhone freeze (`PoseWorker`):** reported in [#872](https://github.com/seedds/nxnative/issues/872) on iPhone after 217–526 s of play (not reproduced on Mac). The reporter noted `DM_PREEMPT=1` avoided it in a 25-minute iPhone session, while the underlying race in `PoseWorker` remains undiagnosed.
- **iOS validation:** not yet tested on an iPhone on this profile.
