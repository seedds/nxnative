# Batman: Arkham Asylum (`0100E870163CA000`)

Update 1.0.1 (`v65536`) over the base game, SDK 14.3.0 (the same `sdk` build as [Batman: Arkham Knight](../0100ACD0163D0000/README.md), `2aef2cff…`), Rocksteady's Unreal Engine 3 (package version `0x243`, licensee `0x15`, Oodle-compressed `.xxx` chunks) with FMOD audio, Scaleform menus and Bink movies (`.bik`), NVN. Modules `main` and `sdk`. RomFS is 2,822 files: 2,674 cooked UE3 packages (`BmGame/CookedSwitch/*.xxx`), `Textures.tfc`, `GlobalShaderCache-Switch.bin`, `Coalesced_<language>.bin` configurations and 42 Bink movies.

Playable on the Mac: two independent bring-ups verified boot and gameplay on 1.0.1 (PR #3338: 147 s headless run drawing 13,200 frames at 8.48 ms p50 frame time / 3.98 ms p50 GPU time with zero missing shader keys, plus a 405 s session of 40,678 frames; PR #3271: 40 minutes from a new game through the intake, first fights and Detective Mode on an M4). iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Compact UE3 shader records | `dm/gfx/tools/shader_split.py` `ue3_compact_shaders` | the title stores no 0x880-byte NVN control sections on disk; see [Files and shaders](#files-and-shaders) |
| a 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | `main` sets a heap of at most the pool − 64 MB, a 3,328 MB heap region ([memory.md](../../../docs/reference/memory.md)); the survey's values, not yet pushed to a failing allocation |
| `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares 0 MB of system resource (`IsVirtualAddressMemoryEnabled` is false) |
| `modules main sdk` | [`game.tsv`](game.tsv) | every module `main` imports from |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game selects near-zero NVN depth (`nvnDeviceSetDepthMode` near = 0 at boot), so offline pipeline bases use `main0_z` |
| `tex_flush_upload on`, `tex_dirty_upload on` | [`game.tsv`](game.tsv) | the engine writes into texture storage from the CPU without flushing: a 405 s session (40,678 frames) counted 234,144 CPU writes into texture storage against 485 re-uploads and 279,046 write-protection faults; with them off, a HUD sprite above an attacking enemy kept stale pool contents. `tex_dirty_upload` needs `tex_flush_upload` on |
| no frame-rate key | [`game.tsv`](game.tsv) | the game's own UE3 limiter is left in charge (`Coalesced_*.bin` cooks `MaxSmoothedFrameRate=30` under `bSmoothFrameRate=TRUE`), so this is a plain 30 fps bring-up |

No game-specific rewriter patch and no host row: `sdk` build `2aef2cff…` takes the SDK-wide rules in `dm/tools/sdk_patches.py`.

## Files and shaders

- **Resolved: the shader split found no shaders.** This title stores no NVN control sections on disk; each shader is a compact record `{u32 region_len, u32 meta_len (< 0x880), u32 0, region, meta}` whose 0x880-byte control section and aligned code area the game's loader (`main+0xcc6194`) builds from a fixed template and up to 0x40 bytes of metadata, including inline two-module VertexA+VertexB vertex programs listed at control `+0x748`. `shader_split.py` (`ue3_compact_shaders` / `ue3_compact_build`) reconstructs them at build time into the working profile's generated tables ([Unreal Engine 3 compact records without stored NVN control sections](../../../docs/reference/shaders.md#unreal-engine-3-compact-records-without-stored-nvn-control-sections)).
- **31,263 shaders** (26,950 pixel, 4,313 vertex including 786 two-module VertexA+VertexB programs) across `GlobalShaderCache-Switch.bin` and the cooked `.xxx` packages. 70 runtime shader pairs dumped via `DM_NVN_SHDUMP` (including three two-module programs) matched byte for byte, and the loader-accurate reader also recovers the 2 pixel shaders (`8a3000a86b82578c` in `Admin_A_CH56_Plants.xxx` and `07d265d3dfda0e3d` in `GlobalShaderCache-Switch.bin`) that fixed-`0x28`/fixed-`0x100` assumptions missed.

## Found on this game

- Compact UE3 shader records without stored NVN control sections, including inline two-module VertexA+VertexB programs: [Unreal Engine 3 compact records without stored NVN control sections](../../../docs/reference/shaders.md#unreal-engine-3-compact-records-without-stored-nvn-control-sections).

## Open

- **No IPA has been run and iOS is untested.** All testing so far is on macOS.
- **Combined profile validation:** `depth_mode zero` (verified in PR #3271) and `tex_flush_upload` / `tex_dirty_upload` (verified in PR #3338) have not yet been re-tested together with the `c6a187a50` compact reader on a fresh Mac pack.
- The 786 two-module vertex programs are generated into the shader library, though PR #3338's recorded route had not yet exercised one at draw time.
- The pipeline prewarm list `pso_manifest.tsv` is empty (first visits to new areas compile pipelines on demand).
- The memory keys are the survey's values (runs settled around 2.1–2.8 GB resident) and have not been pushed to a failing allocation.
