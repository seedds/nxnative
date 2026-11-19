# ENDER LILIES: Quietus of the Knights (`0100CCF012E9A000`)

Update 1.1.5c (v458752) over the base game, SDK 10.4.0. Binary Haze Interactive / Live Wire / Adglobe, Unreal Engine 4 on NVN with FMOD sound and `NintendoSDK_movie` (`subsdk0`) for cinematics.

Plays on the Mac: boots natively, disassembles and passes heap/address region checks, all 4,979 shaders compiled with Apple Metal, and packages a standalone Mac application (`ENDER LILIES - Quietus of the Knights.app`).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 64 GB alias region: `mem_reserve 0x1028000000`, `mem_alias 0x1000000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | Unreal Engine 4: `nninitStartup` reserves a 62 GB address region and its `FMallocBinned3` takes 37 GB; the rewriter gates the allocator and gives the region what the alias region has room for |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk0` is `NintendoSDK_movie` for cutscene video decoding |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0, so pipeline bases take the `main0_z` vertex entries |
| `shader_groups GlobalShaderCache=global EnderLilies/=game` | `game.tsv` | the engine's global shaders from `Engine/GlobalShaderCache-GLSL_SWITCH.bin` in the pak, and the material shaders from the `EnderLilies-Switch.utoc`/`.ucas` IoStore container |

## Files and shaders

- **Files:** RomFS has 14 files (1.05 GB): `EnderLilies-Switch.pak` (584.5 MB), `EnderLilies-Switch.utoc` (1.8 MB), `EnderLilies-Switch.ucas` (428.4 MB), `global.utoc`/`.ucas` and 6 FMOD sound banks (`.bank`).
- **Shaders:** 4,979 unique shaders (1,480 vertex, 2,804 pixel, 673 compute, 22 geometry):
  - 1,581 global shaders in `Engine/GlobalShaderCache-GLSL_SWITCH.bin` inside the pak.
  - Material shaders stored across the chunks of `EnderLilies-Switch.utoc`/`.ucas` (Unreal Engine 4 IoStore TOC v2, Zlib blocks), decoded block-by-block and parsed by `shader_split.py`.
- **Prebuild:** The pipeline archive prebuild generates `applegpu_g13g` (82.9 MB) with no runtime compilation stutters.

## Mac route

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app --romfs bundle`.
- The application is created in the game folder as `ENDER LILIES - Quietus of the Knights.app`.
- Save files live in `~/Library/Application Support/nxnative/0100CCF012E9A000/`.
- Logs write to `~/Library/Logs/nxnative/0100CCF012E9A000.log`.

## Found on this game

General host and tool changes first needed here; each applies to every game:

- **Unreal Engine 4.27 shaders inline in IoStore export bundles**: [Unreal Engine 4.27 shaders inline in IoStore export bundles](../../../docs/reference/shaders.md#unreal-engine-427-shaders-inline-in-iostore-export-bundles).
