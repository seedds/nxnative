# Donkey Kong Bananza (`040074A01BF12000`)

A Switch 2 game: update v458752 over the base game, SDK 20.5.19 built for the Switch 2 (`sdk` names its spec `Ounce`), NVN2. Nintendo EPD's engine (sead / agl / gsys, Phive), modules `main` + `sdk`. **Plays on the Mac** (M4 Pro): three play sessions of about 5 minutes each and scripted runs of up to 15,900 frames, through the opening, the mining tunnel and the areas after it, with the guide movies in their panel, at 60 fps; in busy scenes the game drops itself to 30 fps for a few seconds ([Open](#open)). The first start with no pipeline archive builds the pipelines for about 4 minutes, once. Nothing past the first level's opening areas is tested; iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource: virtual address memory off, the game sets a heap. Without the key the host answers 32 MB, and the SDK's VammManager then maps memory up front (GetInfo 0x21) |
| `app_pool 0x240000000`, `mem_reserve 0x270000000` | `game.tsv` | the Switch 2 leaves games 9 GB of its 12 GB (not measured here); with a Switch 1 pool (3.2 GB) the game's heaps did not fit: `AudioSystemHeap` creation returned null (`main+0x564b48`) |
| `romfs_marker System/Resource/ResourceSizeTable.Product.rsizetable.zs` | `game.tsv` | a file every version has |
| `render_cond on` | `game.tsv` | agl's render buffer binding wraps its clears in render conditions (`SetRenderEnableConditional` at `main+0x12798c`, before the `ClearColor` at `main+0x1279d4`); drawn unconditionally, a clear-only rebinding of the composite target right after the composite draw wiped it before the upscaler read it, and every gameplay frame was black |

## Files

- Built from a dump folder (`base/`, `updates/458752/`, `dlc/040074a01bf13001/`): `build_ipa_from_nsp.py <folder> --platform mac-app` packs it with [`scripts/pack_dump.py`](../../../scripts/pack_dump.py), with no keys; the app has no icon, as a dump has no control data. The update's RomFS is the patched one (80,273 files, 8.6 GB; it drops 42 base files, among them the `Product.100` tables its `Product.200` ones replace), so it is packed on its own. The DLC (add-on content index 1, 8,000 files, 0.9 GB) mounts as `EmeraldRush`.
- Shaders: `Shader/*.Nin_Ounce_NVN2.bfsha.zs` (BNSH) and `Shader/Project.Nin_Ounce_NVN2.release.sarc.zs`. The code is Volta / Ampere's 128-bit encoding (`0x794d` EXIT, `0x7918` NOP), not Maxwell's.
- NVN2 control sections (dumped with `DM_NVN_SHDUMP`): magic `0x98761234`, version `0xc9`; +0x38 the instructions' length (SPH included), +0x40 that rounded to 0x100, +0x44 the code area's (the c1 segment lies between the two), +0x4c the register count, +0x58 compute's thread group. Their own length is at +0x30 in BNSH files only (0xd8; 0xd0 in effects, 0 in agl's BAHS and in memory), so the shader key hashes the first 0x48 bytes with +0x30 zeroed, plus the code area (`hle_nvn.c` `nvn_ctl_lens`, `shader_split.py` `key_ctl`).
- agl's upscaler is an `NDSR` pack, `agl_ndsr.pck` in `Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs`: 16-byte records from +0x10 (control section offset, control length << 4 | type, code offset, code length / 16 << 8 | type) up to a zero one; 30 shaders (3 vertex, 3 pixel, 24 compute: a small network on the tensor cores, HMMA, LDSM, MOVM). `shader_split.py` `ndsr_shaders` reads it, and pairs each vertex record with the pixel record after it.
- The split reads the bfsha / BNSH files, agl's BAHS archives (`agl_common.sharcb` and the others: 1,555 shader records, one more than the archive counts, so the record check allows 1 % of strays) and sead's primitive drawer: 22,852 shaders (with the 30 of the NDSR pack).
- [`geo_mesh.tsv`](geo_mesh.tsv): the game's one vertex / geometry pair (`f9c807930abd1234` with the passthrough kernel `5d537f7630ec3671`), which draws every geometry draw, about 600 a frame; built as a mesh library of 16 triangles a group ([Mesh geometry groups sized to the vertex output](../../../docs/reference/shaders.md#mesh-geometry-groups-sized-to-the-vertex-output)).
- The prewarm seed list, [`pso_manifest.tsv`](pso_manifest.tsv) (3,793 lines: 1,726 shaders, 2,030 render pipelines, 37 compute pipelines), is the Mac's device list of the play sessions before the host read device lists without a seed list ([development.md](../../../docs/development.md#prewarm-lists)): boot, the title, the opening and its guide movies, the mining tunnel and the areas after it, recorded with prewarm off. A start with no device list builds it in about 8 s before the first frame.

## Shaders

The translator decodes Ampere code natively (job `arch: sm86`; [Ampere shader code](../../../docs/reference/shaders.md#ampere-shader-code-switch-2)): all 22,852 shaders translate (chain 6f41726ef36d). NVN2 keeps NVN's texture handle table in c[2] (0x20 + 8 slot) and the graphics stages' storage buffer tables in c[0] (0x110 vertex, 0x510 pixel); compute lays out c[0] per shader from its control section, push constants first ([Ampere shader code](../../../docs/reference/shaders.md#ampere-shader-code-switch-2)). The upscaler pushes (1 / w, 1 / h, w, h) of its target for its graphics passes and the source and destination rectangles for its compute network.

## Rendering

### Native phone aspect (local v458752 build)

`BANANZA_NATIVE_DISPLAY=1` reads the iOS window scene's bounds and scale before
guest entry. `BANANZA_RENDER_HEIGHT` selects a 360–2160 pixel output height
(default 720); width is the closest even number at that scene's aspect. The Duo
fixtures 951×669 and 678×466 points yield 1024×720 and 1048×720. This is the
screen's aspect at a configurable rendering resolution, not one rendered pixel
per physical screen pixel. Geometry is frozen for this launch; a later folding
or scene-size change requires restarting the game. External TV behavior remains
unvalidated.

The build-ID-gated [`patches.py`](patches.py) changes the window-size getters
`0x5bf770/0x5bf780` and screen-render configuration at `0x539cc8/0x5399c0/0x5399d8`.
Project-camera getters `0x45d560/0x45d570` supply the width/height ratio to camera
setter `0x45d5b0`, including the setup naming CityLevelStage, CoreLevelStage and
GroundLevelStage. The per-frame update at `0x4db588` propagates camera `+0x100`
to projection `+0xb0`, extended frustum `+0x1e0`, and recomputes culling and the
matrix. This leaves independent shadow/capture projection constructors alone.
The queued special-camera aspect override at `0x371f4c` remains unchanged.

The root layout renderer at `0x803b60` owns the DrawInfo passed to the on-screen
layout draw (`0x1f363c`), confirmed by runtime probes. Paired hooks at
`0x47188/0x47194` fit projection Y only while the root layout calculates its GPU
uniforms, then restore the exact original projection. They are gated by native
display and DrawInfo vtable `0x23dc990`. Marking the root dirty is necessary to
refresh cached uniforms. View and CPU pane coordinates stay unchanged, so
screen-position readers and selection anchors use the same authored space.
Temporary offscreen cameras remain unchanged.

Authored full-screen pictures would otherwise leave uncovered vertical strips.
The pane Calculate hook at `0x2f8440` extends the height of an explicit list of
37 background/fade picture names identified in this build's BFLYT resources.
It additionally requires the same DrawInfo vtable and original 1920×1080
extents. Two rotated MenuSkill background pictures additionally match their
exact names, 2100×1280/2100×1080 dimensions and −90° rotation. Their parent
rotates +90°, so runtime world matrices are axis-aligned; height extends coverage. Text, character captures, logos and source resources remain unchanged;
the height assignment is idempotent on repeated frames.

Validation: all ten original instructions match; 2076 AArch64 emulation cases
check dimensions, enabled/disabled paths, vtable and picture exclusion,
SP, GPR/SIMD state, flags, nested Calculate restoration and repeated frames.
Full rewriting accepts all ten sites. Mac smoke tests at 1536×1080 (open) and
1572×1080 (closed) verify title, loading, tunnel HUD and Skill menu coverage.
Skill selection corners align with the selected heart and follow movement to
the fist; no purple or exposed blurred strips remain. Both runs report PQ HDR
and no `[RED]` diagnostics. Physical Duo acceptance remains open.
`BANANZA_TEST_SCENE=951x669` with the native-display switch enables the same
geometry on Mac for smoke testing.
The packaged Duo preset uses `BANANZA_RENDER_HEIGHT=1080`: 1536×1080 open,
1572×1080 closed. The tunnel frame recorded on Mac uses 1228×864 main color
targets before the 1536×1080 swapchain; output dimensions are not a promise
that every internal pass renders at that resolution.

The scene renders into 1676×943 targets (R11G11B10F colour), agl's upscaler takes it to 2304×1296, and that is copied to the 2880×1620 window. On the way to the first picture:

1. The upscaler's network did not translate (HMMA and the other tensor instructions): the window kept the output's clear colour (0.3, 0.3, 1).
2. Six effect compute shaders (`d076142721d836a5` and others) did not load: a storage address was a c[0] entry plus an offset from c[1], and the translator took the c[1] term as the base.
3. Black frames: the composite was cleared before the upscaler read it (`render_cond`, [What it needs](#what-it-needs)).
4. Black speckles and white blotches over a dark frame: NaN from the environment cube's mip chain ([Mip-chain pass sampling the level it renders](../../../docs/reference/rendering.md#mip-chain-pass-sampling-the-level-it-renders)). Found with raw dumps (`DM_NVN_SNAPRAW`) of the R11G11B10F targets, `DM_NVN_HILITE` to narrow the shaders (`cca896efc6bb7c21` and `c8637178e2818d18` wrote most of them), then those shaders' MSL instrumented to record their first NaN temporary: a fetch of the prefiltered cube, back through the prefilter to the cube's own mip passes.
5. Woven rock surfaces and holes in the rock: the textures the game loads were untiled in the Tegra X1's order inside a GOB ([The Switch 2's order inside a GOB](../../../docs/reference/nvn-objects.md#the-switch-2s-order-inside-a-gob)). The rock material's albedo (ASTC 8×8) written straight to its output looked right and its BC7 normal map did not, so the BC7 file (`BaseRock000_Pnrm`) was decoded offline in every order of the GOB's address bits.
6. The geometry shader `5d537f7630ec3671` (no vertex output: it writes its primitives to memory with ATOMG / STG) needed LEPC, OUT and ISBERD, and `7529699c7b7e49c0` (ActionLibraryPass compute) MATCH; with them all 468 shaders of the first gameplay minute translate.
7. Black tunnel walls and floor, with only the loose rocks and Donkey Kong drawn (compared with a YouTube capture of the same spot): the terrain's colour pass, about 400 indirect draws, drew nothing.
   - **How the terrain is drawn:** each chunk's depth prepass runs the culling geometry shader `5d537f7630ec3671`. It writes the surviving triangles' indices and adds their count to the chunk's indirect draw record (64 bytes, bound as its storage buffer 0). The colour pass draws from that record.
   - **What was wrong:** the records held instance count 0, and their counts grew frame after frame. Every frame gsys's vertex shader `c9a56f796ea4a4cd` (96 points or more) writes {count 0, one instance} into each record whose address its storage buffer lists. The translator had dropped those stores ([Global memory at an address loaded from memory](../../../docs/reference/shaders.md#global-memory-at-an-address-loaded-from-memory)).
   - **What NVN2 does:** decoding NVN2's indirect draw macro showed that instance count 0 draws nothing and the draw never resets the record.
   - **Now:** the walls, floor and lit passage draw as in the capture, at 60 fps.

## Boot (Mac)

Each step below stopped the boot before its fix:

1. `sdk: no SDK version string`: the version reads `nnSdk-Generic_20_5_19` (`loader.c` `dm_sdk_major` skips the variant name).
2. `GetInfo` 0x21, a Switch 2 info type: the kernel's page size. `nn::ro::ProtectRelro` takes value − 1 as the RELRO alignment mask; 0 aborted (`hle_svc.c` answers 0x1000). Type 28 is Horizon's AliasRegionExtraSize (0, named with 25–27 so that Switch 1 games print no new red line); 29–32 are the Switch 2's remote region: `nnosInitialize` builds `nn::os::detail::RemoteRegionManager`'s address space allocator over [type 29, type 29 + type 30), and `nn::os::GetRemoteMemoryUsage` returns types 31 and 32. Only the GPU driver's boot maps into it (`nv::InitializeGraphics` → `nn::gs::sdk::BootGraphicsSystem` → `BootWithRemoteMemory`), and the host replaces `nv::InitializeGraphics` (the overload with a pool, the one the game calls), so all four are 0: no remote region.
3. NVN2 reports API 231: the game requires major 231, minor ≥ 30 (`main+0x5c61a4`) and creates no graphics system otherwise (null read in its frame loop). `dm_sdk_ounce` (the sdk's `spec: Ounce`) selects 231.30 (`hle_nvn.c`).
4. 197 NVN2 entry points (ray tracing, mesh shaders, DLSS, device commands, HDR window metadata, a ProgramBuilder) were unknown names, so lookups returned null; they are in `names.txt` now, the unimplemented ones going to the red stub. Implemented: the ProgramBuilder (`nvnProgramInitializeFromBuilder` = Initialize + SetShaders), `QueueBuilderSetPrimaryQueue`, `CommandBufferSetRecordingMode` / `SetGeometryProcessingMode` / `SetComputeOverlapMode`.
5. Switch 2 SDK calls through services the host does not serve: `hid::InitializeJcms` and the Joy-Con mouse sensor (`hle_hid.c`), `oe::GetCurrentGraphicsScale` (`hle_oe.c`), `audio::IsDeviceOutputVolumeAutoTuneModeEnabled` (`hle_audren.c`), `streamplay` (GameShare, `hle_misc.c`), `vi` HDR / VRR / extended swap interval (`dm/gfx/hle_vi.c`).
6. `nv::InitCheck` (from `main+0xe19d04`, the start of a video decoder, after the first punch) tested the SDK's own flag, which `nv::InitializeGraphics` sets and the host's version of it does not: `hle_nv.c` answers it from the host's record. The same function then starts a video decoder: the Switch 2's `nn::codec` video API and sync point events, served on the host ([`nn::codec` video decoders taken over](../../../docs/reference/movies.md#nncodec-video-decoders-taken-over)).

## Open

- **Video decoding:** the guide movies are VP9 and play (a 180-picture clip on loop in the tutorial's panel, read back as NV12). The player also imports the VP8, H.264 and AV1 decoders: VP8 and H.264 are served too (checked by `t_codec_video` only), AV1 is not ([`nn::codec` video decoders taken over](../../../docs/reference/movies.md#nncodec-video-decoders-taken-over)).
- **The gameplay picture:** the mining tunnel matches a YouTube capture of the same spot ([Rendering](#rendering) 7); nothing past the tunnel's first minute has been compared.
- Full shader build (about 75 minutes on the Mac from an empty cache): 22,852 of 22,852 shaders on chain 6f41726ef36d (SUATOM decoded; 22,850 on chain 3c66991eb439). Chain 9a0726336e9d had 22,834: the Metal compiler refused 11 (too deeply nested jump tables) and 5 used BRXU or REDUX ([Ampere shader code](../../../docs/reference/shaders.md#ampere-shader-code-switch-2)); 2 still do not translate (0x593).
- `oe::GetCurrentGraphicsScale`: which tier the console reports in which mode is not measured (the host answers 0 handheld, 1 console). The game uses it as a quality tier, not for correctness: a byte of its graphics settings and a renderer parameter of 0, 9 or 13 (`hle_oe.c`).
- The 9 GB pool.
- The terrain's stores through loaded pointers keep reaching memory pools the host learns only afterwards (12 in a 17,700-frame session); each such miss is checked against the uniform shadows that relied on it, and none so far covered a written page ([Global memory at an address loaded from memory](../../../docs/reference/shaders.md#global-memory-at-an-address-loaded-from-memory), Open).
- **Frame rate in busy scenes:** the game picks its own present interval: 3 (20 fps) in its cutscenes, and 1 ↔ 2 (60 ↔ 30 fps) by its frame-rate controller (`main+0x204a18`; settings at `0x703f72b200`: interval 1 to 3, margin -0.2, 2 late frames to go up, 6 to come back). It measures each frame from its start (`+0xf0`) to the later of its submission and the GPU's end (`+0x128`, ticks) and wants interval 2 from 1.2 vsyncs (20 ms). On the Mac (M4 Pro) that measure sits at 17 ms a frame (p90 24): the host's translation (CPU, 5-9 ms) before the GPU's 10-13 ms. The upscaler's HMMA through `simdgroup_matrix` ([Ampere HMMA through simdgroup_matrix](../../../docs/reference/shaders.md#ampere-hmma-through-simdgroup_matrix)) cut the switches by about a fifth; the geometry runs' dependency check, quadratic in the run's length ([Geometry dependency checks skip read-only pairs](../../../docs/reference/shaders.md#geometry-dependency-checks-skip-read-only-pairs)), was about 8 % of the submitting thread, and making it linear cut them from 30 to 12 on the same scripted route. Entering a level stalled for 1.3 s on GPU drains, one or two per geometry draw ([CPU reads after a drain wait only for later writes](../../../docs/reference/rendering.md#cpu-reads-after-a-drain-wait-only-for-later-writes)); one CPU-bound build frame of 195-280 ms remains. Measured without `sample` attached: it suspends the process and makes the submitting thread look stalled. Its geometry draws are mesh draws ([Mesh geometry groups sized to the vertex output](../../../docs/reference/shaders.md#mesh-geometry-groups-sized-to-the-vertex-output)): translation 9.9 → 7.2 ms a frame, GPU p50 11.8 → 9.0 ms on the scripted route. Open: the rest of the translation time in the busiest views (argument encoding, render pass setup), where translation and the GPU together still miss a vsync now and then.
- The NVN2 device values the driver reads from the GPU at run time (17 items: scratch scale factors, L2 size, texture layers and sizes, linear stride alignment, pool page size, …) are still the Switch 1 ones; the constant ones are NVN2's ([The Switch 2's device values](../../../docs/reference/nvn-objects.md#the-switch-2s-device-values)).
- iOS untested.

## Found on this game

- [Switch 2 HDR scanout on Apple EDR displays](../../../docs/reference/rendering.md#switch-2-hdr-scanout-on-apple-edr-displays): capability negotiation and PQ presentation verified in Mac gameplay; physical iOS display validation remains open.

- Everything under [Boot](#boot-mac) is new: the first Switch 2 game here.
- [Ampere shader code (Switch 2)](../../../docs/reference/shaders.md#ampere-shader-code-switch-2): the translator's Ampere frontend, its scoreboard model and the IPA / texture / load semantics found on this game's menu (green boxes instead of text; no text); the tensor instructions, NVN2 compute's c[0] layout and push constants, MUFU SIN / COS in turns, and the storage base rule found in its gameplay.
- [The Switch 2's order inside a GOB](../../../docs/reference/nvn-objects.md#the-switch-2s-order-inside-a-gob): the textures from its files.
- [`nn::codec` video decoders taken over](../../../docs/reference/movies.md#nncodec-video-decoders-taken-over): the guide movies.
- [The Switch 2's system tick is 31.25 MHz](../../../docs/reference/frame-rate.md#the-switch-2s-system-tick-is-3125-mhz): the frame-rate controller held 20 fps.
- [Ampere HMMA through simdgroup_matrix](../../../docs/reference/shaders.md#ampere-hmma-through-simdgroup_matrix): agl's upscaler network, 2-6.5 ms a frame emulated with shuffles, 1.1-1.6 ms on the matrix hardware.
- [The Switch 2's device values](../../../docs/reference/nvn-objects.md#the-switch-2s-device-values): NVN2's own `nvnDeviceGetInteger` answers, read from the game's sdk.
- [The device's prewarm list unread without a seed list](../../../docs/reference/shaders.md#the-devices-prewarm-list-unread-without-a-seed-list): compile stalls every session until the device list was read.
- [Global memory at an address loaded from memory](../../../docs/reference/shaders.md#global-memory-at-an-address-loaded-from-memory): the terrain's indirect draw records, reset by a gsys vertex shader's stores through loaded pointers, and the host's learned write set for such stores.
- [Mip-chain pass sampling the level it renders](../../../docs/reference/rendering.md#mip-chain-pass-sampling-the-level-it-renders): the environment cube's NaN texels.
- [Geometry dependency checks skip read-only pairs](../../../docs/reference/shaders.md#geometry-dependency-checks-skip-read-only-pairs): runs of 56 geometry draws, checked pairwise, took 8 % of the submitting thread.
- [CPU reads after a drain wait only for later writes](../../../docs/reference/rendering.md#cpu-reads-after-a-drain-wait-only-for-later-writes): the level opening drained the GPU for each geometry draw of an indirect-count multi-draw, a hitch of 1.3 s.
- [Mesh geometry groups sized to the vertex output](../../../docs/reference/shaders.md#mesh-geometry-groups-sized-to-the-vertex-output): 388-byte vertices, over Metal's mesh limit at 32 triangles a group.
- [Ampere shader code (Switch 2)](../../../docs/reference/shaders.md#ampere-shader-code-switch-2): SUATOM (0x593) in two agl technique compute shaders, the last two of the package's 22,852 that did not translate.
- [Shader libraries loaded when the program is made](../../../docs/reference/shaders.md#shader-libraries-loaded-when-the-program-is-made): a new area's 3 to 7 libraries loaded on the submit thread, up to 4 s with the drive slowed.


## HDR port validation (v458752)

**Current:** the host has an opt-in EDR scanout path (`DM_HDR=1`). Mac startup and mining-tunnel gameplay negotiate HDR and present Rec.2020 PQ on an EDR display. Physical iOS display output remains unvalidated. See [Switch 2 HDR scanout](../../../docs/reference/rendering.md#switch-2-hdr-scanout-on-apple-edr-displays).

The v458752 SDK's `DefaultDisplay::QueryHdrStatus` at `sdk+0x55bd60` accepts output formats 0 and 1 and computes `on = (format == 1)`. Main calls it at `main+0x2037b4`, keeping the bool at renderer `+0x1cd` and peak at `+0x1d0`; it ignores the returned format. Paper white is queried at `+0x2037d4`. With renderer HDR enabled (`+0x1ce`) and available (`+0x1cd`), the window path at `main+0x199d98` selects NVN RGB10A2 (`0x3d`) when the float flag (`+0x1cf`) is zero, or RGBA16F (`0x29`) when it is one. The current port targets the RGB10A2 mode. Main submits gamut enum 1 at `+0x2036f4` and standard/extended luminance metadata at `+0x199e60`/`+0x199e70`.

The SDK NVN lookup table leads to `WindowInitialize` at `sdk+0x5efdc0`; this implementation does not use the builder color-space field at `+0x24`. Its platform window initializer at `+0x5e2480` maps formats through the jump table at `+0xba0751`: `0x25` to native texture kind 1, `0x29` to kind 5, `0x3d` to kind 4. `NativeWindow::InitializeImpl` at `+0x56a180` maps kind 4 to native color space 1 and kind 5 to linear color space 2; the latter explicitly receives an 80-nit unit at `+0x56a26c`. Thus the source format is part of the SDK's scanout contract, not merely inferred from its bit depth. The float fallback uses scRGB primaries; its visual match for this game's float mode remains open, so keep the game's float flag zero for this port.


Startup HDR is already enabled without a game-code patch: `main+0x5bf110` stores 1 at the startup config's offset `0x1d`, and `main+0x5bf0ac` stores 0 at offset `0x1e`. The constructor's copy at `+0x5c2724` places these at renderer `+0xa5`/`+0xa6`; `+0x5c62d4` transfers them to `+0x1ce`/`+0x1cf`. Later `main+0x30f9cc` can update the enabled byte from the render manager's flags at `+8`, bit 12; a saved or runtime option can therefore still change the mode. Acceptance should check both the `vi HDR: enabled` capability log and `present HDR: Rec.2020 PQ` scanout log.

Validation in this port: `t_present_hdr` passed 36 checks with Metal API validation; `t_present_crop` passed 88 checks. The Apple layer helper also passed an iOS 26 SDK syntax compile with `-Wall -Werror`. The scoped `scripts/regress.py check --games 040074A01BF12000 --quick --no-smoke` passed: 23,638/23,638 shaders, no new failures, changed MSL or interface differences. The scope is the opt-in Switch 2 HDR/native-display path and the one locally available compatible game; runtime validation is the separate Mac smoke described above.

## Local package content verification

The supplied update v458752 contains `Product.320.Nin_Ounce_NVN2.atbl.byml.zs`;
the matching main build is registered as 3.2.0. The original `v458752` display
label produced a synthetic `0.458752.0` title-screen version. Correcting that
profile label does not change the code or data selected for packaging.
The supplied DLC `040074a01bf13001/0` is mounted as EmeraldRush; its title-screen
version is independently reported as DLC 1.0.0. This content set produces
23,638 shaders, including the DLC additions to the original profile.

## Halo and presentation investigation (2026-11-02)

- [Bloom upsampling retains higher mip levels](../../../docs/reference/rendering.md#bloom-upsampling-retains-higher-mip-levels): live gold-deposit frame 43298 proves the renderer removed the mip levels requested by bloom's reconstruction samplers. The complete 2D mip-chain snapshot fixes that binding; GPU regression passes, gameplay comparison is in progress.
- [Covered key windows do not trigger display-link rescue](../../../docs/reference/frame-rate.md#covered-key-windows-do-not-trigger-display-link-rescue): visibility precedes focus loss in the user's Space transitions. The watchdog now distinguishes this from a genuinely silent visible window.
- Open: remaining 60/30 transitions and the exact contribution of CPU submission work and guest GPU timestamp granularity. Do not describe this change as a 60 FPS lock or a fix for every low-resolution effect.

## Duo launcher resolution and intermittent stalls (2026-11-02)

The launcher exposes output heights 720, 1080, 1440, 1800, 2008 and 2160.
Width is still derived by `dm_native_display_freeze` from the current scene
aspect, rounded to an even pixel; these choices do not stretch the scene.
2008 gives 2854x2008 on the open 2853x2007 panel. The game still controls its
internal dynamic resolution. Remove `BANANZA_RENDER_HEIGHT` from bundled and
Documents env files to allow the launcher choice to apply.

Physical Duo log `duo-15fps.log` records GPU median increasing from about 14 ms
to 39-42 ms and displayed frame cadence of 66.7 ms. Thermal state remains 0;
no pipeline or shader compilation occurs in the sustained slow intervals.
The user reports transient scene-only horizontal corruption during camera
rotation. Root cause remains open. A diagnostic comparison disables
`DM_NVN_ENC_THREAD` through Documents/env.txt, keeping output resolution and
HDR unchanged; this is not a confirmed fix for either symptom.

Follow-up at 1440p: the user reproduced corruption with the Metal HUD disabled,
and with both `DM_NVN_ENC_THREAD=0` and `DM_PRESENT_LINK=0`. Sequential captures
also show older poses reappearing and rectangular discontinuities at 60 FPS.
The recorded slow interval has actual encoder GPU time increasing (F5400:
8.25 ms render; F6000: 27.14 ms; F6300: 39.51 ms), not just a slow HUD counter.
Metal API validation was confirmed enabled on Duo and reported no API errors
in the collected session; this does not rule out synchronization faults.

Diagnostic `DM_TEST_PRESENT_CLASSIC=1` with `DM_PRESENT_LINK=0` isolates drawable
handoff: game rendering remains Metal 4, a shared event orders the classic
present queue's source read, and its completion releases the source ticket.
The present uses the same HDR conversion and bilinear scaling as the HDR Metal
4 path. It is opt-in, not a confirmed fix; display-link presents keep the
existing path because their early source release assumes a single GPU queue.
The gated real-GPU test (`t_present_alloc classic`) verifies that the read waits
for the producer, the ticket remains held, and the resulting pixels match.

The user subsequently reported no visible corruption with this diagnostic at
2854x2008, but stationary-scene performance still decays from about 14 ms to
30 ms and then 44 ms. A short Home gesture can restore 60 FPS, sometimes via
20 -> 30 -> 60 FPS over two gestures. This separates the remaining performance
failure from the visible drawable corruption; neither platform root cause is
established yet.

Device Metal System Traces `duo-stationary-20fps.trace` and
`duo-stationary-recovered.trace` record different GPU performance-state indices:
the slow sample stays at 1, while the recovered sample is predominantly 2.
These indices are not frequency measurements. Depth-zero application fragment
intervals average about 322 us (33,037 intervals) versus 108 us (50,040), and
vertex intervals about 91 us versus 32 us. These are aggregate interval means,
not matched individual draws. Encoder profiling in the same stationary scene
also reports roughly unchanged attachment traffic with greatly increased
execution time. A subsequent launch-only `DM_RT=0` comparison removes the
host's adaptive time-constraint thread policy while preserving 2008p, HDR and
classic presentation reproduced the failure: F5400 reports 43.30 ms render,
0.78 ms compute/copy, 46.16 ms span, and no shader compilation or uploads.
`duo-rt-off-slow.trace` again stays in GPU performance state 1 for the entire
sample (15.31 s). Removing the real-time policy is therefore not a solution.
Do not treat the immediate improvement after a restart as evidence for that
policy's involvement. The next opt-in comparison,
`DM_TEST_PRESENT_NO_DURATION=1`, removes only the classic drawable's minimum
duration hint; guest/acquire pacing and source synchronization remain intact.
This is diagnostic, not a production pacing change or a confirmed fix.
That comparison also reproduced the failure after about 150 s: F7800/F8100
render time is about 42 ms, with thermal state 0 and no compilation/uploads.
The user describes recovery only on refocus once the slower state is entered.
Both test overrides were removed from Documents/env.txt for subsequent launches;
the subsequent fixed-cadence launch uses neither override. The classic handoff remains
enabled. The detailed Instruments state table labels the slow state `Minimum`
and the fast state mostly `Medium`; both carry `is-induced=Yes` and the narrative
`due to active device conditions`. Whether that narrative represents a real
diagnostic override or the tool's classification remains under investigation.

A bounded DVT Medium-state request coincided with recovery, but the user
confirmed opening the game menu during that test: it cannot establish causality.
Separate DVT sessions reported no active condition even while the request was
held, so that listing is not reliable evidence of global condition state. The
requests were stopped and explicitly cleared.

The next diagnostic uses `DM_PRESENT_INTERVAL=display` at 60 Hz, ignoring the
game's requested 1/2/3 present intervals. After more than six minutes it had
not reproduced the 40–45 ms lock-in; render time was about 19 ms, with remaining
frame drops. The user confirms substantially improved behavior. This remains a
diagnostic deviation, not proof of correct simulation timing or a production
fix. A 29 ms frame interval alone does not demonstrate a 30 FPS cap: this mode
already ignores the game's interval requests. Output is 2854x2008, while the
main 3D passes are 2283x1606 and reconstructed to output resolution.

After the user removed the Peltier cooler, the log reached thermal state 2
(serious), with F28200 render 25.81 ms and GPU span 28.90 ms. The fanless trace
records GPU state 1 throughout 10.63 s; the preceding fixed-cadence trace mixes
state 1 (7.69 s) and 2 (2.61 s). Thermal pressure and the lower performance
state are consistent with throttling, but these indices do not quantify clock
frequency or prove the cause of every slowdown. A subsequent log starts a new
run and still reports thermal 2; its low menu GPU cost is not a gameplay recovery
measurement. Artifacts: `duo-fanless-latest.log`, `duo-fanless.trace`,
`duo-fanless-after-trace.log` in the device-install work directory.

At 1080p fanless, `duo-1080-timing.log` separates the remaining frame cost:
F5700–7200 runs about 55 FPS with thermal 2. Submission translation consumes
11.9–12.7 ms CPU per frame (wall time 12.0–12.8 ms), at measured cycles/CPU-time
of 1.47–1.52 GHz; F7200 has 74.16 million instructions and 4,783 draws per frame.
GPU encoder work is around 8–9 ms, but first-start-to-last-end spans 15–17 ms.
These overlapping timings must not be added. The final classic bilinear command
buffer has p50 about 0.7 ms. Frame p50 is 17.2–17.4 ms, p95 about 22.5 ms, and
24–28 of each 300 displayed frames miss the nominal cadence. This establishes
substantial CPU translation cost, not a residual 30 FPS present cap; attribution
of individual gaps between GPU encoders remains open. No shader compilation or
texture uploads explain the sampled long frames.

The launcher default must also be applied when bypassing its UI: `applySaved`
now exports defaults as well as saved choices. Otherwise an unset 2008p
launcher default falls through to the native host's 720p fallback. The storage
regression checks the environment value without persisting a new preference.

### Upstream integration on Duo (2026-11-02)

Updated the local base from `cd859c38a8f8` to `163a990515bd`, retaining the
native-display patches, HDR, bloom feedback fix, launcher defaults and classic
presentation diagnostic. The local modifications have a tar/patch/hash backup
in the game's device-install work directory. All 2,076 display stub emulation
cases, native geometry, launcher storage, HDR (36 checks), classic handoff
(4 checks), present allocators, feedback, storage-write and running-uniform
shadow tests pass. All 23,638 shaders compile on chain `313942e6b187`, with zero
shader regressions against `b7cd656abd7c`. Validation is scoped to the requested
Bananza integration; other games were not rebuilt or run.

The updated app was signed and installed over the same bundle ID. First launch
confirms the new commit/chain, the saved 1080p choice, closed-display output
1572x1080 and HDR enabled. The device went out of focus during pipeline prewarm;
this launch does not yet establish gameplay performance or visual acceptance.
`DM_PRESENT_INTERVAL=display` and `DM_TEST_PRESENT_CLASSIC=1` remain enabled for
comparison with the preceding run. Logs and receipt: `upstream-first-launch.log`
and `upstream-integration.json` in the device-install work directory.
