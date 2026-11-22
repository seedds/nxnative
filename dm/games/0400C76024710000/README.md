# PRAGMATA (`0400C76024710000`)

A Switch 2 game: update v196608, SDK 20.5.15 built for the Switch 2 (`Ounce`), NVN2. Capcom's RE Engine, with Havok and Wwise. Modules `main` and `sdk`; `subsdk0` and `subsdk1` (C / libc) are not loaded. On an M4 Pro Mac (48 GB), it reaches the title, a new game's intro (the ship, the tower and the lunar base, last run on chain `79939b004bb5`) and, with the resulting save, the station's first rooms (chain `d24f05cf9d78`). [Reported iPhone runs](#reported-iphone-runs) reached the opening with a larger alias region.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB pool. On the Mac, the host's alias default applies. In the first 15 s, the game's regions reach alias +38.41 GiB, including a 32 GiB request. The profile's 4.38 GiB iPhone reservation is too small for that request. See [reported iPhone runs](#reported-iphone-runs) |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `modules main sdk` | `game.tsv` | `main` takes nothing from the two subsdks that the host does not supply |
| `romfs_marker re_chunk_000.pak`, `zip_store .pak:.metallib` | `game.tsv` | the RomFS is three RE Engine PAKs (12.86 GB) |
| `shader_groups re_chunk=sdf` | `game.tsv` | the shaders are [SDF definitions inside the PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 |

There is no game-specific patch and no rewriter cut. The profile is the NSP script's guess from the survey, unchanged; only v196608's build row exists, and the base game has not been run.

## Shaders

36,779 in [`shader_map.tsv`](shader_map.tsv): 18,406 pixel, 10,829 vertex, 6,880 compute, 539 mesh, 62 domain, 33 hull, 30 geometry. On chain `dfd5e3048b0b` all 36,779 build and none is lost; a full Mac build with nothing cached took 82 minutes with 10 jobs on the M4 Pro (other work running beside it for part of the time), 39 minutes with the Metal compiles cached.

- Resolved on chain `ed49d5de5fe9`: pixel shader `c4ac3ca598cea32e` built, but a render pipeline with it ended Apple's compiler service, with each of its three vertex shaders (`02721ccb12f9f810`, `467d5368b677b114`, `7a8f2da68bafd2d3`): `XPC_ERROR_CONNECTION_INTERRUPTED`, a fatal error under `llvm::MCObjectStreamer::emitInstruction`. [A jump Apple's GPU compiler cannot encode](../../../docs/reference/shaders.md#a-jump-apples-gpu-compiler-cannot-encode-a-tested-bit-0s-mask): the mask of a tested bit 0 is now written without its compare. Checked offline and through the compiler service on its translation (macOS 26.6.2, M4 Pro), and in a full Mac build on macOS 27.2 on an M6 (chain `02a94e855ee3`, this change on an earlier main): all 36,779 shaders, the pipeline prebuild of all 48,957 pairs with none failed, and a run on a fresh save from the first-start screens through the title into the intro on the ship, with no new red line. A `prebuild_failed.tsv` written before lists the pairs until the library's stamp changes.
- Resolved on this chain: 65 shaders with [FRND of a half](../../../docs/reference/shaders.md#frnd-of-a-half-frndf16), 24 pixel shaders reading [one handle as a texture and as an array](../../../docs/reference/shaders.md#one-handle-read-as-a-texture-and-as-an-array) (their alpha test entry points failed with them), 2 domain shaders under [the one-lane subgroup rewrite](../../../docs/reference/shaders.md#subgroup-operations-rewritten-for-a-one-lane-subgroup), and 21 on three [instruction forms](../../../docs/reference/shaders.md#ampere-forms-met-on-pragmata-tld-through-a-uniform-register-i2f-of-an-immediate-suatomcas); on `dfd5e3048b0b`, pixel shader `4a9d4507eea6c837` with [IPA at an offset and the barycentric coordinates](../../../docs/reference/shaders.md#ipa-at-an-offset-and-the-barycentric-coordinates).

## Half .RELU validation

PR #2049 (2026-11-18): the local 36,779-shader package contains 2,960 HFMA2 `.RELU` instructions across 729 compute and 640 pixel shaders. The local package has no HFMA2.MMA or HMUL2 `.RELU` instructions.

Offline comparison on M4 Pro, macOS 27.2.1 and Metal 32023.922: all 1,369 affected shaders and both Mac hosts built on baseline `a739027a` (chain `98a64ccff492`) and candidate chain `dee90fcd0718`. The candidate changes all 1,369 base MSL sources and 640 pixel variants. Reflection differences are limited to the chain stamp and MSL source line numbers in the 729 compute loop tables. Pixel varying inputs agree and no shader resources or entry points were lost. Both builds used an isolated map with package routing matched to the local split. Gameplay and frame time were not checked.

## Driving it on the Mac

`python3 scripts/build_ipa_from_nsp.py <folder> --platform mac` builds the Mac shaders and the host and prints the run command. In a window, `DM_PSO_PREPARE=0` skips the prepare step, which otherwise builds 48,957 pipelines before the first frame. The title is up about 90 s after boot; through `DM_CTL`, `A` (title), `A` (New Game), `A` (Standard), then left and `A` (Yes) start a new game, which plays its intro (the ship, the tower, the lunar base). With a save from an earlier run the same presses put the player in the station, where the left stick walks; which menu entries they chose there was not looked at.

Measured on the M4 Pro, in a 1920x1080 window: 60 fps at the title, 44 to 52 fps walking through the station's first rooms over four runs, with pipelines still compiling in the background. Whether the game's own target there is 60 or 30 is not known.

## Reported iPhone runs

On an A20 Pro iPhone (12 GB, iOS 27.2), host `b3fe7770`, v196608 with DLC `0400c76024711001`, both update-only and complete extracts faulted at `main+0x788f0` (`SIGSEGV`, address `0x10`) at startup. With `DM_ALIAS_SIZE=42949672960` in `Documents/env.txt`, the reporter reached the opening, logged tens of thousands of frames and saw no crash in five sessions. The override reserves 40 GiB of virtual address space. The guest memory pool is still 9 GiB. [Issue #2935](https://github.com/seedds/nxnative/issues/2935)

In console mode at thermal state 2, with a 60 Hz link and no pipeline archive, the initial report measured a 29.91 ms median frame time, 34.68 ms p95, 28.82 ms median GPU time and a 2.8–3.3 GB footprint. The judder samples had 13–33 empty display-link ticks per 300 and zero new pipelines in the long frames. The display link defaults to showing queued frames in order. `DM_PRESENT_FIFO=0` selects the newest queued frame, and the reporter did not test it. The shipped `pso_manifest.tsv` has no seed entries.

On the same phone with iOS 27.3 beta 2, host `8a06d2550d5a`, chain `e641898384f7`, and the same override, the log confirmed that the 32 GiB request succeeded at alias +0.07 GiB. About seven minutes into the opening, Metal reported `kIOGPUCommandBufferCallbackErrorOutOfMemory` and the host stopped after GPU access was revoked. In a preceding 300-frame window the residency set logged 2,879 MB of additions and the footprint was 2,727 MiB. GPU p50 near the failure ranged from 34.9 to 37.7 ms. The override gets past startup on this device. Sustained play, the cause of the residency peak and the frame cadence remain open. [Device follow-up](https://github.com/seedds/nxnative/issues/2935#issuecomment-7073458809)

## Found on this game

- [HFMA2.MMA and half .RELU NaN handling](../../../docs/reference/shaders.md#hfma2mma-the-half-pair-fma-on-the-tensor-pipe): see [validation](#half-relu-validation); no picture symptom has been traced to these instructions.
- [Raw global memory lookups that start from their last entry](../../../docs/reference/shaders.md#raw-global-memory-lookups-that-start-from-their-last-entry) — on the M6 (macOS 27.2, game v196608), the CPU-compatible address build reached the title screen, rendered both characters and ran the `251426e9d44fd558_b` culling shader with GPU stores and atomics.
- [Compute loops that outlive the process](../../../docs/reference/shaders.md#compute-loops-that-outlive-the-process): compute-only containment with attributable cap-hit logging, including helpers. The queue mismatch's root cause and binding remain open in [#2664](https://github.com/seedds/nxnative/issues/2664); this does not close it or establish a fresh reproduction.
- [A registered build without the profile's DLC stops at the shader split](../../../docs/reference/packing.md#a-registered-build-without-the-profiles-dlc-stops-at-the-shader-split): resolved; `re_dlc_000.pak` holds 45 of the game's shaders, which are now the game's rows in `shader_map.tsv` (36,779 shaders with the DLC, 36,649 without).
- [Compute dispatches past the pipeline's thread limit](../../../docs/reference/shaders.md#compute-dispatches-past-the-pipelines-thread-limit): resolved, found offline (no M1 or M2 run): 73 of its 255 compute shaders above 384 threads (69 of them 1,024 threads) took more registers on `applegpu_g13s` / `applegpu_g14g` than their size allows, so on an M1 or M2 their dispatches ran nothing; with the local size on the kernel all fit, and the M4 Pro's code is unchanged. What each draws is not known.
- [H.264 parameter sets past the eighth argument](../../../docs/reference/movies.md#h264-parameter-sets-past-the-eighth-argument): its opening movies.
- [More than 1,024 mapped runs in a sparse pool](../../../docs/reference/nvn-objects.md#more-than-1024-mapped-runs-in-a-sparse-pool): the GPU hang 3 to 4 minutes into the intro.
- [Pages changed under a buffer the GPU still uses](../../../docs/reference/nvn-objects.md#pages-changed-under-a-buffer-the-gpu-still-uses): absurd indirect dispatches and a hang while it maps and unmaps sparse pages.
- [A sparse pool's runs replaced in two steps](../../../docs/reference/nvn-objects.md#a-sparse-pools-runs-replaced-in-two-steps): mesh shaders `765e6d9d2588ce70` and `5aab38bb5ba67ef7` bound to a dummy buffer for a frame.
- [A sparse pool's streamed textures read 0 at their first read](../../../docs/reference/rendering.md#a-sparse-pools-streamed-textures-read-0-at-their-first-read): a dark frame at the cut from the tower to the tablet.
- [Queue builder size getters](../../../docs/reference/nvn-objects.md#queue-builder-size-getters): read after `SetDefaults` as it makes its queue.
- [Float blend clamp](../../../docs/reference/nvn-objects.md#float-blend-clamp-nvncommandbuffersetfloatblendclamp): it sets 1.
- [A sync never fenced since its Initialize is signalled](../../../docs/reference/nvn-objects.md#a-sync-never-fenced-since-its-initialize-is-signalled).
- [The provoking vertex is the first](../../../docs/reference/rendering.md#the-provoking-vertex-is-the-first): it sets mode 0.
- [A 1D array texture uploaded as its layer count of rows per layer](../../../docs/reference/rendering.md#a-1d-array-texture-uploaded-as-its-layer-count-of-rows-per-layer): its light-profile lookup table.
- [A loop exit's subgroup block run by the lanes of every iteration together](../../../docs/reference/shaders.md#a-loop-exits-subgroup-block-run-by-the-lanes-of-every-iteration-together): resolved, found offline: compute shader `607ceeefb9478700`'s warp-aggregated `SURED.MIN` mixed keys, and 11 other compute shaders have the same shape. No picture has been traced to it. Rechecked offline on 2026-11-17 after the entry-condition guard regression: the four reduction ballots in `607ceeefb9478700` return to their inner loops, SPIR-V validates, and its Mac and iOS libraries compile. Gameplay for this correction has not been retested.
- [Touch gestures recognized from the touch points](../../../docs/reference/services.md#touch-gestures-recognized-from-the-touch-points).
- [SDF definitions in RE Engine PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks).
- [Pool tables that held only their own texture type](../../../docs/reference/nvn-objects.md#pool-tables-that-held-only-their-own-texture-type): Cabin's monitor face (sprites read as 2D arrays) and every cube map read by handle.
- [Integer outputs told by the pipeline's targets](../../../docs/reference/shaders.md#integer-outputs-told-by-the-pipelines-targets).
- [Mesh shader per-primitive system values: the primitive count overwritten, the primitive id lost](../../../docs/reference/shaders.md#mesh-shader-per-primitive-system-values-the-primitive-count-overwritten-the-primitive-id-lost).
- [Mesh shaders that also address their vertices as records](../../../docs/reference/shaders.md#mesh-shaders-that-also-address-their-vertices-as-records).
- [Mesh shaders that read their own outputs back](../../../docs/reference/shaders.md#mesh-shaders-that-read-their-own-outputs-back).
- [Threadgroup stores in warp-synchronous code](../../../docs/reference/shaders.md#threadgroup-stores-in-warp-synchronous-code).
- [FSETP / ISETP pairs on their own accumulator, and IMAD's B](../../../docs/reference/shaders.md#fsetp--isetp-pairs-on-their-own-accumulator-and-imads-b).
- [The shared compare-and-swap loop retries with the value it got](../../../docs/reference/shaders.md#the-shared-compare-and-swap-loop-retries-with-the-value-it-got).
- [Ampere forms with a handle in a register, and SURED's other operations](../../../docs/reference/shaders.md#ampere-forms-with-a-handle-in-a-register-and-sureds-other-operations).
- [Ampere BMSK and the read of a covered pixel](../../../docs/reference/shaders.md#ampere-bmsk-and-the-read-of-a-covered-pixel).
- [A guard band cull in every vertex shader (0x80d)](../../../docs/reference/shaders.md#a-guard-band-cull-in-every-vertex-shader-0x80d).
- [MEMBAR is a fence, not a barrier](../../../docs/reference/shaders.md#membar-is-a-fence-not-a-barrier).
- [NVN2 compute's second push block holds buffer entries](../../../docs/reference/shaders.md#nvn2-computes-second-push-block-holds-buffer-entries): the hang at frame ~1,300.
- [NVN2 graphics stages' buffer banks (c\[25\] to c\[29\]) are push blocks](../../../docs/reference/shaders.md#nvn2-graphics-stages-buffer-banks-c25-to-c29-are-push-blocks): the black title and structures: the root cause.
- [Uniform words read through a buffer address (cx)](../../../docs/reference/shaders.md#uniform-words-read-through-a-buffer-address-cx).
- [Pixel `c0 + 0x620`: the sample index and GLSLC's sample position table](../../../docs/reference/shaders.md#pixel-c0--0x620-the-sample-index-and-glslcs-sample-position-table).
- [Global memory at a storage buffer's address on some paths only](../../../docs/reference/shaders.md#global-memory-at-a-storage-buffers-address-on-some-paths-only).
- [Bindless constant buffer handles in the hardware's layout](../../../docs/reference/shaders.md#bindless-constant-buffer-handles-in-the-hardwares-layout).
- [Handles a shader computes index the pools](../../../docs/reference/shaders.md#handles-a-shader-computes-index-the-pools): 65,241 deleted texture reads in 8,630 shaders, the intro a saturated cyan.
- [A handle loaded from memory on one path only](../../../docs/reference/shaders.md#a-handle-loaded-from-memory-on-one-path-only): compute shader `02e1292114121308` did not load.
- [64-bit image max and min (SURED.MAX.U64)](../../../docs/reference/shaders.md#64-bit-image-max-and-min-suredmaxu64).
- [One handle read as a texture and as an array](../../../docs/reference/shaders.md#one-handle-read-as-a-texture-and-as-an-array): 24 pixel shaders did not compile.
- [FRND of a half (FRND.F16)](../../../docs/reference/shaders.md#frnd-of-a-half-frndf16): 65 shaders did not translate.
- [Ampere forms met on PRAGMATA](../../../docs/reference/shaders.md#ampere-forms-met-on-pragmata-tld-through-a-uniform-register-i2f-of-an-immediate-suatomcas): 21 shaders.
- [Subgroup operations rewritten for a one-lane subgroup](../../../docs/reference/shaders.md#subgroup-operations-rewritten-for-a-one-lane-subgroup): two domain shaders.
- [A base the GPU compiler cannot encode cost the whole offline archive](../../../docs/reference/shaders.md#a-base-the-gpu-compiler-cannot-encode-cost-the-whole-offline-archive): three bases with pixel shader `c4ac3ca598cea32e`.
- [Reads past a storage buffer bound by address](../../../docs/reference/shaders.md#reads-past-a-storage-buffer-bound-by-address): the colour wash in the R53 hangar ([GitHub issue #2618](https://github.com/seedds/nxnative/issues/2618)). The GI voxeliser `ef4ad01151918469` read past the one run mapped at the base of a sparse pool; its reads past the bound memory now give 0. Open: the hangar retest (no save near it here).
- [IPA at an offset, and the barycentric coordinates](../../../docs/reference/shaders.md#ipa-at-an-offset-and-the-barycentric-coordinates): pixel shader `4a9d4507eea6c837` did not translate; NVN2's sample position table is signed from the pixel centre.

## Open

- `[RED] nvnCommandBufferSetFloatBlendClamp 1 …: Metal stores infinity`, once: the game asks for float blend results that overflow to become the largest finite value, which Metal cannot do ([Float blend clamp](../../../docs/reference/nvn-objects.md#float-blend-clamp-nvncommandbuffersetfloatblendclamp)); a platform limit, and no frame has been seen to differ.
- `[RED] stage 5 cb1 has no uniform buffer bound: shader 9d5de89b049f5915_b`, once: not a fault of the host. The game leaves the slot unbound, and the shader bounds each read of a slot by the size in its handle (`index < size`, the handle row the host writes: 0 for an unbound slot), so the read gives 0 as it does on the console. The line reports the unbound slot, which the host cannot tell from a missed bind; it stays.
- `[RED] queue 4: an event wait has held it 8.1 s`, once in one run, as the game left the title: the wait sat inside a frame of 11.9 s, 11.6 s of it 29 pipelines compiled at first use (the run skipped the prepare step), so the producer's submission was late, not missing. Not seen in the two runs after it.
- The shader under [Shaders](#shaders).
- How long the prepare step's 48,957 pipelines take on a first start has not been measured. Combat and later areas remain open.
