# Digimon Story Time Stranger (`010062E01FE0C000`)

Update 1.2.2 (`v262144`), SDK 20.5.21. Media.Vision's MVGL engine on **NVN**, with CRIWARE audio/movies (`subsdk0` is `NintendoSDK_movie`). Modules `main`, `subsdk0`, and `sdk`.

Tested on Mac (Apple M6, macOS 27.2.1, Xcode 27.2) in both docked (`DM_OPMODE=1`) and handheld (`DM_OPMODE=0`) modes. Playable through the logos, autosave warning, privacy policy, DLC popups, opening 3D cutscene, protagonist selection, and early story gameplay (an 18.5-minute / 33,311-frame interactive docked session with sound, saving cleanly to slot `0000`, at 30.0 fps, GPU p50 6.05–7.25 ms, peak resident 2,795 MB / GPU 1,386 MB / alias mapped 1.75 GB, 0 `[RED]` lines, 0 missing shaders, 0 audio underruns; handheld mode verified at 30.0 fps, GPU p50 4.76 ms, 0 `[RED]` lines). iOS device validation is noted below.

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
