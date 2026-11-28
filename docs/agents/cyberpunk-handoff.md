# Cyberpunk 2077 Mac handoff

The Switch 2 build `0400DDB020648000`, update `v262144`, reaches gameplay on Apple M6 with 16 GB of memory, macOS 27.2.1 (26B131) and Xcode 27.2. The existing Nomad save opens by the garage. Select Continue, then press B when the game prompts. Standing gameplay usually runs at 26–27 FPS. The 60 FPS target and the first scene's 50 ms GPU budget remain open.

## Reproduce the benchmark

The local build root is `/Volumes/S790/nsp/cyberpunk 2077/0400DDB020648000_build`. Its provisional profile and detailed game notes are in `profile/0400DDB020648000/`. The installed build used the build-gated `patched_v262144_shader_cache` and the local Mac shader library `shader/cache/b-71df44ea44be.img` with 6,754 shaders. If the chain changes, rebuild that profile's shaders, then rebuild the host with the current chain. Check the host's startup line for the loaded chain and shader count. The first-stage and second-stage null controls had the same cause. Another REDengine worker saw a record before initialization finished. The local profile's `patches.py` delays publication until the record is ready, gated to main build `afd670b6ba27b9e68d268e601bf91acd`.

The landing rebase built all 6,754 Mac shaders with chain `bcda968cb25a`. The new pack is in `shader/landing-rebase/`, and its matching `shader_map.tsv`, `c1_full.tsv` and `cb_extent.tsv` are in `shader/landing-profile/0400DDB020648000/`. Point `DM_MSL_DIR` and `DM_NVN_SHMAP` there when testing the rebuilt host. The FPS and GPU numbers below are from the earlier live build and its saved settings.

Keep the save's Quality setting and 1920×1080 temporal MetalFX output. Internal rendering resolution changes in response to frame time. Compare throughput over full windows and compare encoder samples with similar input sizes and draw counts. Leave the original save in `run/save` for interactive use. Use a copy of `run/stream-nomad-map-batch-0-20261127-223701/save` for repeatable captures. The copied-save route used the following inputs.

```sh
export DM_KEYS='f2100:B~f15,f2400:B~f15,f3000:Plus~f20,f3300:T0.125x0.40~f9,f3330:A~f20,f4810:B~f15'
export DM_WINDOW=1 DM_METAL_HUD=1 DM_FOCUS_KEEP=1 DM_NVN_DLSS_BILINEAR=0
export DM_NVN_TSV=/private/tmp/nomad-frame.tsv DM_NVN_TSV_CALLS=0
```

Keep just one game process active during timing. For a frame-level breakdown, use `DM_NVN_GPUPROF=every:61` and parse the log with `dm/tools/gpuprof.py`. Use `DM_NVN_PASSDUMP` on one representative frame for draw states and pass boundaries. Read the corresponding `@frame_ms`, `@gpu_ms`, `@upload_gpu_ms`, `@draws` and `@faults` rows. Upload queue time can overlap main GPU time. On the original Mac, `/private/tmp/nxnative-nomad-queue-shadow-open.sh` opened the live HUD and `/private/tmp/nxnative-nomad-capture-runner.py` ran isolated copies. The scripts and generated host variants are local diagnostics. They can be reconstructed from the game note, [build instructions](../development.md) and [debugging guide](05-debugging.md).

## Gameplay

- The clean HUD run `run/hud-saved-renderer-20261128-013055` on the original save averaged 26.6 FPS after B, with GPU p50 35.9 ms and p90 39.2 ms in frames 5500–6099.
- The later ledger `run/stream-nomad-geometry-ledger-20261128-014451` put all rendering at 22.1 ms and three- and four-colour depth passes at 16.1 ms median. The most expensive compute dispatch was `8503e78f76f77003_b` at 3.6 ms, with a 35×20×14 grid of 4×4×4 threads. The dispatch ledger adds about 8–9 ms across several kernels. Encoder and individual dispatch intervals may overlap. The game's dominant geometry state writes depth with GEQUAL and stencil enabled. Seven depth snapshot encoders together cost about 0.5 ms per frame.
- Four same-target continuations split when bindless pool entries actually referred to the active attachments. Removing that guard was unsafe. Snapshotting all colour attachments used up the timing gain. The retained depth snapshot fixed the one-draw-pass collapse. Across the saved-game comparison, 1,832 passes became 211. The guard still matters for real feedback reads.
- The separate MetalFX queue removed the recurring wait between scaler stages in the live comparison. GPU p90 moved from 85.6–102.0 ms to 39.0–39.5 ms. The game raised its median internal input size from 640×360 to approximately one million pixels. This accounts for much of the improved pacing and higher effective image quality. The same game inputs by themselves did not reproduce the stall in a standalone scaler replay.
- Leave the current compiler optimization level, sparse copy barriers and render-area threshold in place. An O2 rebuild of the two hottest fragments and the main compute shader, exact range tracking for about six sparse copies per frame, a smaller scissor area and a render-clear alternative for color zeroing each failed to yield a repeatable FPS gain. Native SIMD OR for a shuffle sequence and caching additional shader lookups also failed to improve frame throughput. The run directories are `stream-nomad-optlevels-*`, `stream-nomad-copyrange-*`, `stream-nomad-drawstate-*` and the fragment and compute experiments listed in the local game note.

## Loading

- In the clean HUD run the first scene frame took 288.1 ms wall, 56.3 ms main GPU and 9.25 ms upload GPU. Its 2,135 draws and roughly 21,500 tracked writes make it distinguishable from the preceding loading frames. Warm captures of that transition had already built the relevant pipelines.
- The largest recorded idle interval at this transition was before the CPU committed the next command buffer. Mapping was a measured part of that delay. Reusing already backed shadow pages in two copied-save launches reduced shadow copying from 58.3 to 27.0 ms, the largest late submission interval from 215.0 to 150.1 ms and first scene wall time from 314.7 to 224.2 ms. Main GPU time was 56.0 to 48.7 ms in that pair. The clean HUD run still crossed 50 ms. Direct sparse deswizzle separately cut the first two frames' summed upload GPU time from 15.8 to 11.4 ms in a matched pair.
- For the next loading investigation, capture a CPU timeline on the late submission interval and use the same live replay to time the first three-colour depth passes. This distinguishes remaining guest streaming delays from the work after submission. The 150 ms interval and the 50 ms GPU spike need separate measurements.
- For steady rendering, isolate fragment execution in the dominant depth-writing passes and measure the raw buffer lookup cost in the material shaders at matching internal resolutions. A fragment timestamp already includes fixed-function and attachment work. Resizing targets or dropping attachments would invalidate the comparison. For compute, start with the 8503 dispatch's storage and SIMD work, using the original reflection and checking its outputs at the Metal boundary.

## Verification boundary

Macros and offsets in the local shader-cache patch are tied to the observed game build. The shared work covers NVN, shader translation, packing and host services. Mac gameplay evidence is for the parked Nomad scene and the earlier opening route. iOS and sustained 60 FPS remain open. The original title-screen work is [PR #3293](https://github.com/seedds/nxnative/pull/3293), linked to [issue #3194](https://github.com/seedds/nxnative/issues/3194).
