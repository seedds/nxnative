# Tomodachi Life: Living the Dream (`010051F0207B2000`)

Update 1.0.4 (v262144) and 1.0.2 (v131072) over the base game, SDK 18.x / 19.x (build `651781821e2c2e0b2bdfe905e8676f56`). Nintendo EPD's proprietary engine (`sead` / `agl` / `gsys`, NintendoWare NVN). Modules `main` and `sdk`. Plays on iPad (tested on iPad Pro M4) and iPhone at full 60 fps. IPA size is about 7.3 GB.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `pad_layout full` | [`game.tsv`](game.tsv) | 3D gamepad layout: left stick navigates the island, right stick rotates the camera, face buttons interact with islanders and UI |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | memory reserve and application heap pool for high-memory simulation titles |
| `mem_used 0x7a00000` (122 MB) | `game.tsv` | rewritten module footprint (`main` and `sdk` text/bss regions) |
| `render_cond on` | `game.tsv` | the game wraps occlusion queries and render clears in render conditions |
| `tex_flush_upload on`, `tex_dirty_upload on`, `tex_dyn_slots on` | `game.tsv` | dynamic texture upload streaming and texture descriptor slot management |

## Rendering

- **Resolved: terrain ground (clover & grass) rendering as flat pale green, dark foundations, and alternate green/white checkerboard borders around building plots.**
  - **Symptom**: Out of the box, outdoor grass and clover terrain displayed as an untextured, flat pale green surface. Constructible plots on the island were surrounded by alternating white and green checkerboard grid cells, and foundations beneath buildings showed dark green discoloration prior to generation.
  - **Root Cause**:
    1. *Dynamic Descriptor Array Indexing*: The floor model fragment shaders (`alFloorModel`, etc.) dynamically sample `fp_t2d_cb3_0[texture_index]` based on layer attributes calculated at runtime from uniform block `fp_c3`. On Switch NVN, texture arrays in descriptor sets can index into higher layer indices (e.g. index $\ge 32$). Under Metal, `fp_t2d_cb3_0` was declared as `array<texture2d<float>, 32>`, and the translator clamped dynamic indices using `min(uint(idx), 31u)`. This clamped clover and grass lookups to index 31 (untextured soil/base color), losing all surface texture detail.
    2. *Metal Argument Buffer ABI Invalidation*: Expanding the binding array declaration in the shader JSON descriptor from 32 to 256 broke the Argument Buffer ABI contract between the host runtime encoder and the Metal driver. The host rewriter encoded 32 texture descriptors (640 bytes at offset 0), whereas the Metal compiler expected 256 texture descriptors (5,120 bytes), shifting all subsequent uniform buffer offsets (`fp_c3` offset shifted from 0x280 to 0x1400). As a result, the shader read garbage floats for building bounding attributes, producing dark green floor discoloration and alternating white-and-green checkerboard bounding frames.
  - **Fix (Authentic Texture Synthesis with Zero JSON ABI Modification)**:
    - Kept the shader JSON binding array strictly untouched at 32 (`array<texture2d<float>, 32>`), fully preserving the host-to-device Metal Argument Buffer memory layout and eliminating the checkerboard grid artifacts.
    - Extracted authentic Nintendo Switch RomFS textures (`clover_alb_test_bh1` and `grass_alb`), quantized them into 256-color palettes and compact 256×256 lookup tables embedded directly into the 21 terrain fragment shaders (`tl_clover_tex`, `tl_grass_tex`, `tl_clover_pal`, `tl_grass_pal`).
    - Routed dynamic terrain samples to procedural synthesis handlers (`synth_terrain_sample4`, `synth_terrain_sample2_xy`, `synth_terrain_sample2_xz`, `synth_terrain_sample1_x`) with bilinear filtering, height perturbation, roughness, and ambient occlusion synthesis.
    - Scaled derivative computations (`* ((base_var >= 256) ? 0.08 : 1.0)`) to eliminate edge filtering artifacts across texture boundaries.
    - Bumped the PSO cache tag in `DmIOS` from `dm_shader_prp2_%@_%@.done` to `dm_shader_prpA_%@_%@.done` (`pso_archivA`) and recomputed Mach-O SHA-256 CodeDirectory signatures to force an invalidation of stale pipeline caches on device.

## Input

- The 3D controller layout (`pad_layout full`) maps all in-game island exploration, camera controls, and UI navigation cleanly to physical gamepads (Xbox, DualSense, MFi) or on-screen controls.
- Island selection menus, dress-up, dialogue advances, and apartment room interactions respond accurately without dropped input frames.

## Saves

- Saves are stored in the application container under `Documents/save/`.
- When migrating existing saves manually from a Switch console or emulator, any temporary `save.work/` transaction folder must be removed before launching the game, otherwise the host HLE filesystem may treat the directory as an incomplete write transaction.

## Files and Shaders

- **RomFS**: 66,343 files (6.3 GB). The profile's `romfs_marker` is `Font/Font_CNzh.Nin_NX_NVN.bfarc.zs`.
- **Shaders**: 8 NVN shader archives in `RomFS/Shader/` (`GameUber...bfsha.zs`, etc.). The 21 terrain shaders requiring the authentic RomFS texture patch:
  `04113a560257ee6b`, `18c5cb4bb782109d`, `1b611e4eb2a2d0eb`, `1c02d9296302896b`,
  `358322f6f26e6d96`, `386554429a1a034f`, `4ca189a07a1aa868`, `68db88d8da4ac679`,
  `785a6a3f24d63f1a`, `86afdf616cc10282`, `9fe3c18082c29456`, `adba070f5fbc166a`,
  `ba3b9faf4765c4e2`, `bb62dd4dd4f94aa7`, `c708d30df2e72607`, `c7f9b506209fc76d`,
  `c8a5491bc1cc4584`, `e058e49682d7a8bf`, `e14d113383ee9b3b`, `fc26c834f1a49900`,
  `fd5ca80abbd46d62`.

## Found on this game

General host and tool insights first identified on this game:

- **Preserving Metal Argument Buffer ABI on texture array overrides**: Modifying shader binding JSON metadata (`array<texture2d<float>, N>`) without updating the host NVN descriptor pool encoder causes argument buffer stride shifts, misaligning subsequent uniform buffers (`fp_c3`) and causing visual artifacts or crashes. Inlining quantized textures into MSL allows extending texture assets without perturbing argument buffer strides.
- **Optimized NSP scanning in packaging scripts**: `folder.rglob("*.nsp")` in packing utilities traversed nested pack output directories containing over 100,000 extracted files, causing severe hangs during scan phases. Pruning `*_pack` directories restores instantaneous NSP discovery.

## Open

- Touchscreen-native gesture mapping (pinch-to-zoom / swipe rotation) alongside standard gamepad emulation.
