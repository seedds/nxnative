# PRAGMATA (`0400C76024710000`)

A Switch 2 game: update v196608, SDK 20.5.15 built for the Switch 2 (`Ounce`), NVN2. Capcom's RE Engine, with Havok and Wwise. Modules `main` and `sdk`; `subsdk0` and `subsdk1` (C / libc) are not loaded. It plays on the Mac: the title, a new game's intro (the ship, the tower and the lunar base; last run on chain `79939b004bb5`) and, with the save those runs left, walking through the station's first rooms (chain `d24f05cf9d78`). Tested on an M4 Pro (48 GB) only.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB pool; `main` sets a heap. On the Mac the host's own alias default applies: in its first 15 s the game's regions reach alias +38.41 GB, one request of 32 GB among them, so the profile's 4.38 GB alias region (the iPhone layout) is not enough as it is |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `modules main sdk` | `game.tsv` | `main` takes nothing from the two subsdks that the host does not supply |
| `romfs_marker re_chunk_000.pak`, `zip_store .pak:.metallib` | `game.tsv` | the RomFS is three RE Engine PAKs (12.86 GB) |
| `shader_groups re_chunk=sdf` | `game.tsv` | the shaders are [SDF definitions inside the PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 |

There is no game-specific patch and no rewriter cut. The profile is the NSP script's guess from the survey, unchanged; only v196608's build row exists, and the base game has not been run.

## Shaders

36,779 in [`shader_map.tsv`](shader_map.tsv): 18,406 pixel, 10,829 vertex, 6,880 compute, 539 mesh, 62 domain, 33 hull, 30 geometry. On chain `dfd5e3048b0b` all 36,779 build and none is lost; a full Mac build with nothing cached took 82 minutes with 10 jobs on the M4 Pro (other work running beside it for part of the time), 39 minutes with the Metal compiles cached.

- **Open:** pixel shader `c4ac3ca598cea32e` builds, but Apple's GPU compiler cannot encode it (`LLVM ERROR: Failed to encode.`), in the bases of its three pairs (vertex shaders `02721ccb12f9f810`, `467d5368b677b114`, `7a8f2da68bafd2d3`): [A base the GPU compiler cannot encode cost the whole offline archive](../../../docs/reference/shaders.md#a-base-the-gpu-compiler-cannot-encode-cost-the-whole-offline-archive) has what is known of the trigger. What it costs the game: a packed app lost its whole pipeline archive to it and built all 48,957 bases on its first start, which `pso_offline.py` now avoids by leaving the three pairs to the device; a run from the build folder logs one to three `[RED] pipeline prebuild: the base of … failed: … XPC_ERROR_CONNECTION_INTERRUPTED` lines on its first start (the compiles in flight when the compiler service ends fail with it, two to four other pairs, and are built at their draws), records them in `prebuild_failed.tsv` and does not try again; a draw with the shader would be skipped, and none has been seen from the title into the station's first rooms (macOS 27.2, M4 Pro).
- Resolved on this chain: 65 shaders with [FRND of a half](../../../docs/reference/shaders.md#frnd-of-a-half-frndf16), 24 pixel shaders reading [one handle as a texture and as an array](../../../docs/reference/shaders.md#one-handle-read-as-a-texture-and-as-an-array) (their alpha test entry points failed with them), 2 domain shaders under [the one-lane subgroup rewrite](../../../docs/reference/shaders.md#subgroup-operations-rewritten-for-a-one-lane-subgroup), and 21 on three [instruction forms](../../../docs/reference/shaders.md#ampere-forms-met-on-pragmata-tld-through-a-uniform-register-i2f-of-an-immediate-suatomcas); on `dfd5e3048b0b`, pixel shader `4a9d4507eea6c837` with [IPA at an offset and the barycentric coordinates](../../../docs/reference/shaders.md#ipa-at-an-offset-and-the-barycentric-coordinates).

## Driving it on the Mac

`python3 scripts/build_ipa_from_nsp.py <folder> --platform mac` builds the Mac shaders and the host and prints the run command. In a window, `DM_PSO_PREPARE=0` skips the prepare step, which otherwise builds 48,957 pipelines before the first frame. The title is up about 90 s after boot; through `DM_CTL`, `A` (title), `A` (New Game), `A` (Standard), then left and `A` (Yes) start a new game, which plays its intro (the ship, the tower, the lunar base). With a save from an earlier run the same presses put the player in the station, where the left stick walks; which menu entries they chose there was not looked at.

Measured on the M4 Pro, in a 1920x1080 window: 60 fps at the title, 44 to 52 fps walking through the station's first rooms over four runs, with pipelines still compiling in the background. Whether the game's own target there is 60 or 30 is not known.

## Found on this game

- [H.264 parameter sets past the eighth argument](../../../docs/reference/movies.md#h264-parameter-sets-past-the-eighth-argument): its opening movies.
- [More than 1,024 mapped runs in a sparse pool](../../../docs/reference/nvn-objects.md#more-than-1024-mapped-runs-in-a-sparse-pool): the GPU hang 3 to 4 minutes into the intro.
- [Pages changed under a buffer the GPU still uses](../../../docs/reference/nvn-objects.md#pages-changed-under-a-buffer-the-gpu-still-uses): absurd indirect dispatches and a hang while it maps and unmaps sparse pages.
- [A sparse pool's runs replaced in two steps](../../../docs/reference/nvn-objects.md#a-sparse-pools-runs-replaced-in-two-steps): mesh shaders `765e6d9d2588ce70` and `5aab38bb5ba67ef7` bound to a dummy buffer for a frame.
- [Queue builder size getters](../../../docs/reference/nvn-objects.md#queue-builder-size-getters): read after `SetDefaults` as it makes its queue.
- [Float blend clamp](../../../docs/reference/nvn-objects.md#float-blend-clamp-nvncommandbuffersetfloatblendclamp): it sets 1.
- [A sync never fenced since its Initialize is signalled](../../../docs/reference/nvn-objects.md#a-sync-never-fenced-since-its-initialize-is-signalled).
- [The provoking vertex is the first](../../../docs/reference/rendering.md#the-provoking-vertex-is-the-first): it sets mode 0.
- [Touch gestures recognized from the touch points](../../../docs/reference/services.md#touch-gestures-recognized-from-the-touch-points).
- [SDF definitions in RE Engine PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks).
- [Integer outputs told from float bits](../../../docs/reference/shaders.md#integer-outputs-told-from-float-bits).
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
- [IPA at an offset, and the barycentric coordinates](../../../docs/reference/shaders.md#ipa-at-an-offset-and-the-barycentric-coordinates): pixel shader `4a9d4507eea6c837` did not translate; NVN2's sample position table is signed from the pixel centre.

## Open

- `[RED] nvnCommandBufferSetFloatBlendClamp 1 …: Metal stores infinity`, once: the game asks for float blend results that overflow to become the largest finite value, which Metal cannot do ([Float blend clamp](../../../docs/reference/nvn-objects.md#float-blend-clamp-nvncommandbuffersetfloatblendclamp)); a platform limit, and no frame has been seen to differ.
- `[RED] stage 5 cb1 has no uniform buffer bound: shader 9d5de89b049f5915_b`, once: not a fault of the host. The game leaves the slot unbound, and the shader bounds each read of a slot by the size in its handle (`index < size`, the handle row the host writes: 0 for an unbound slot), so the read gives 0 as it does on the console. The line reports the unbound slot, which the host cannot tell from a missed bind; it stays.
- `[RED] queue 4: an event wait has held it 8.1 s`, once in one run, as the game left the title: the wait sat inside a frame of 11.9 s, 11.6 s of it 29 pipelines compiled at first use (the run skipped the prepare step), so the producer's submission was late, not missing. Not seen in the two runs after it.
- The shader under [Shaders](#shaders).
- How long the prepare step's 48,957 pipelines take on a first start has not been measured. A `pso_manifest.tsv` has not been recorded. Combat, later areas, a long session and iOS are not tested; the alias region above has no iPhone answer.
