# Donkey Kong Bananza (`040074A01BF12000`)

A Switch 2 game: update v458752 over the base game, SDK 20.5.19 built for the Switch 2 (`sdk` names its spec `Ounce`), NVN2. Nintendo EPD's engine (sead / agl / gsys, Phive), modules `main` + `sdk`. **In bring-up, not playable:** on the Mac it renders the mode-select menu and the first gameplay scene (the tutorial cave), a dim mine with its gold ore, grass and light shafts ([Open](#open)).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource: virtual address memory off, the game sets a heap. Without the key the host answers 32 MB, and the SDK's VammManager then maps memory up front (GetInfo 0x21) |
| `app_pool 0x240000000`, `mem_reserve 0x270000000` | `game.tsv` | the Switch 2 leaves games 9 GB of its 12 GB (not measured here); with a Switch 1 pool (3.2 GB) the game's heaps did not fit: `AudioSystemHeap` creation returned null (`main+0x564b48`) |
| `romfs_marker System/Resource/ResourceSizeTable.Product.rsizetable.zs` | `game.tsv` | a file every version has |
| `render_cond on` | `game.tsv` | agl's render buffer binding wraps its clears in render conditions (`SetRenderEnableConditional` at `main+0x12798c`, before the `ClearColor` at `main+0x1279d4`); drawn unconditionally, a clear-only rebinding of the composite target right after the composite draw wiped it before the upscaler read it, and every gameplay frame was black |

## Files

- Packed from a dump folder (`base/`, `updates/458752/`, `dlc/040074a01bf13001/`) with [`scripts/pack_dump.py`](../../../scripts/pack_dump.py): the update's RomFS is the patched one (80,273 files, 8.6 GB; it drops 42 base files, among them the `Product.100` tables its `Product.200` ones replace), so it is packed on its own. The DLC (add-on content index 1, 8,000 files, 0.9 GB) mounts as `EmeraldRush`.
- Shaders: `Shader/*.Nin_Ounce_NVN2.bfsha.zs` (BNSH) and `Shader/Project.Nin_Ounce_NVN2.release.sarc.zs`. The code is Volta / Ampere's 128-bit encoding (`0x794d` EXIT, `0x7918` NOP), not Maxwell's.
- NVN2 control sections (dumped with `DM_NVN_SHDUMP`): magic `0x98761234`, version `0xc9`; +0x38 the instructions' length (SPH included), +0x40 that rounded to 0x100, +0x44 the code area's (the c1 segment lies between the two), +0x4c the register count, +0x58 compute's thread group. Their own length is at +0x30 in BNSH files only (0xd8; 0xd0 in effects, 0 in agl's BAHS and in memory), so the shader key hashes the first 0x48 bytes with +0x30 zeroed, plus the code area (`hle_nvn.c` `nvn_ctl_lens`, `shader_split.py` `key_ctl`).
- agl's upscaler is an `NDSR` pack, `agl_ndsr.pck` in `Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs`: 16-byte records from +0x10 (control section offset, control length << 4 | type, code offset, code length / 16 << 8 | type) up to a zero one; 30 shaders (3 vertex, 3 pixel, 24 compute: a small network on the tensor cores, HMMA, LDSM, MOVM). `shader_split.py` `ndsr_shaders` reads it, and pairs each vertex record with the pixel record after it.
- The split reads the bfsha / BNSH files, agl's BAHS archives (`agl_common.sharcb` and the others: 1,555 shader records, one more than the archive counts, so the record check allows 1 % of strays) and sead's primitive drawer: 22,852 shaders (with the 30 of the NDSR pack).

## Shaders

The translator decodes Ampere code natively (job `arch: sm86`; [Ampere shader code](../../../docs/reference/shaders.md#ampere-shader-code-switch-2)): 22,751 of the 22,822 shaders translate. NVN2 keeps NVN's texture handle table in c[2] (0x20 + 8 slot) and the graphics stages' storage buffer tables in c[0] (0x110 vertex, 0x510 pixel); compute lays out c[0] per shader from its control section, push constants first ([Ampere shader code](../../../docs/reference/shaders.md#ampere-shader-code-switch-2)). The upscaler pushes (1 / w, 1 / h, w, h) of its target for its graphics passes and the source and destination rectangles for its compute network.

## Rendering

The scene renders into 1676×943 targets (R11G11B10F colour), agl's upscaler takes it to 2304×1296, and that is copied to the 2880×1620 window. On the way to the first picture:

1. The upscaler's network did not translate (HMMA and the other tensor instructions): the window kept the output's clear colour (0.3, 0.3, 1).
2. Six effect compute shaders (`d076142721d836a5` and others) did not load: a storage address was a c[0] entry plus an offset from c[1], and the translator took the c[1] term as the base.
3. Black frames: the composite was cleared before the upscaler read it (`render_cond`, [What it needs](#what-it-needs)).
4. Black speckles and white blotches over a dark frame: NaN from the environment cube's mip chain ([Mip-chain pass sampling the level it renders](../../../docs/reference/rendering.md#mip-chain-pass-sampling-the-level-it-renders)). Found with raw dumps (`DM_NVN_SNAPRAW`) of the R11G11B10F targets, `DM_NVN_HILITE` to narrow the shaders (`cca896efc6bb7c21` and `c8637178e2818d18` wrote most of them), then those shaders' MSL instrumented to record their first NaN temporary: a fetch of the prefiltered cube, back through the prefilter to the cube's own mip passes.
5. Woven rock surfaces and holes in the rock: the textures the game loads were untiled in the Tegra X1's order inside a GOB ([The Switch 2's order inside a GOB](../../../docs/reference/nvn-objects.md#the-switch-2s-order-inside-a-gob)). The rock material's albedo (ASTC 8×8) written straight to its output looked right and its BC7 normal map did not, so the BC7 file (`BaseRock000_Pnrm`) was decoded offline in every order of the GOB's address bits.
6. The geometry shader `5d537f7630ec3671` (no vertex output: it writes its primitives to memory with ATOMG / STG) needed LEPC, OUT and ISBERD, and `7529699c7b7e49c0` (ActionLibraryPass compute) MATCH; with them all 468 shaders of the first gameplay minute translate.

## Boot (Mac)

Each step below stopped the boot before its fix:

1. `sdk: no SDK version string`: the version reads `nnSdk-Generic_20_5_19` (`loader.c` `dm_sdk_major` skips the variant name).
2. `GetInfo` 0x21, a Switch 2 info type: the kernel's page size. `nn::ro::ProtectRelro` takes value − 1 as the RELRO alignment mask; 0 aborted (`hle_svc.c` answers 0x1000). Type 28 is Horizon's AliasRegionExtraSize (0, named with 25–27 so that Switch 1 games print no new red line); 29 and 30 are still answered 0 with a red line: `nnosInitialize` builds an address space allocator over [type 29, type 29 + type 30), which region is unknown.
3. NVN2 reports API 231: the game requires major 231, minor ≥ 30 (`main+0x5c61a4`) and creates no graphics system otherwise (null read in its frame loop). `dm_sdk_ounce` (the sdk's `spec: Ounce`) selects 231.30 (`hle_nvn.c`).
4. 197 NVN2 entry points (ray tracing, mesh shaders, DLSS, device commands, HDR window metadata, a ProgramBuilder) were unknown names, so lookups returned null; they are in `names.txt` now, the unimplemented ones going to the red stub. Implemented: the ProgramBuilder (`nvnProgramInitializeFromBuilder` = Initialize + SetShaders), `QueueBuilderSetPrimaryQueue`, `CommandBufferSetRecordingMode` / `SetGeometryProcessingMode` / `SetComputeOverlapMode`.
5. Switch 2 SDK calls through services the host does not serve: `hid::InitializeJcms` and the Joy-Con mouse sensor (`hle_hid.c`), `oe::GetCurrentGraphicsScale` (`hle_oe.c`), `audio::IsDeviceOutputVolumeAutoTuneModeEnabled` (`hle_audren.c`), `streamplay` (GameShare, `hle_misc.c`), `vi` HDR / VRR / extended swap interval (`dm/gfx/hle_vi.c`).
6. `nv::InitCheck` (from `main+0xe19d04`, the start of a video decoder, after the first punch) tested the SDK's own flag, which `nv::InitializeGraphics` sets and the host's version of it does not: `hle_nv.c` answers it from the host's record. The same function then calls `nn::codec::video::Initialize`, which crashes in the SDK ([Open](#open)).

## Open

- **Video decoding:** the Switch 2's `nn::codec` video API (about 70 entry points: H.264, AV1, VP8 and VP9 decoders, decoder image pools, NV12 / RGBA readback) is not served; the game crashes at `nn::codec::video::Initialize` after the first punch.
- **The gameplay picture:** compared with no reference capture; Donkey Kong was not seen yet on the first screen (the camera faces a rock wall).
- **Shaders not translated (71, measured before the upscaler work; LEPC and MATCH have come since):** 55 load through addresses read from memory (no storage buffer to turn the access into); the others use instructions the frontend lacks (0x958, 0x3c4, 0x593). The package has not been counted again.
- No full shader build yet (boot keys only), so no library stamp, `c1_full.tsv` or `cb_extent.tsv`.
- `oe::GetCurrentGraphicsScale`: which tier the console reports in which mode is not measured (the host answers 0 handheld, 1 console).
- GetInfo types 29 and 30, and the 9 GB pool.
- The NVN2 device values (`nvnDeviceGetInteger`) are still the Switch 1 ones.
- iOS untested.

## Found on this game

- Everything under [Boot](#boot-mac) is new: the first Switch 2 game here.
- [Ampere shader code (Switch 2)](../../../docs/reference/shaders.md#ampere-shader-code-switch-2): the translator's Ampere frontend, its scoreboard model and the IPA / texture / load semantics found on this game's menu (green boxes instead of text; no text); the tensor instructions, NVN2 compute's c[0] layout and push constants, MUFU SIN / COS in turns, and the storage base rule found in its gameplay.
- [The Switch 2's order inside a GOB](../../../docs/reference/nvn-objects.md#the-switch-2s-order-inside-a-gob): the textures from its files.
- [Mip-chain pass sampling the level it renders](../../../docs/reference/rendering.md#mip-chain-pass-sampling-the-level-it-renders): the environment cube's NaN texels.
