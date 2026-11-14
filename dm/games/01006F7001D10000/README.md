# Monster Boy and the Cursed Kingdom (`01006F7001D10000`)

Update 1.0.5 (v327680), SDK 8.3.1, on NVN (398 shaders in 202 `.glslcobj` programs). Modules `main`, `subsdk0` (NintendoSDK_movie, which plays the logo and intro videos) and `sdk`. Game Atelier's own engine (FDG Entertainment publishing). The standard host and rewriter run it; the provisional profile needed no change.

On the Mac (2026-11-06, Apple M1 Pro, 16 GB, macOS 26.7.3, a gameplay check of about 7 minutes): the publisher logo video, the intro video, the title menu and gameplay, at 60 fps (frame time p50 16.65 / p95 19.03 ms, no dropped frames) with audio and no `[RED]` line.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 3.25 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout, which holds: `main` sets a heap of at most the pool − 64 MB − its 134 MB of bss |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk0` is Nintendo's movie library; the videos play through it |
| `.glslcobj` reader | `dm/gfx/tools/shader_split.py` | the shaders are in neither BNSH archives nor whole GLSLC outputs; see [Shaders](#shaders) |

## Files

- RomFS: 565 files, 4.5 GB. `DATA/` holds the levels and sprites in `PACK<n>.S` / `.T` / `.M` files, `bgm/` the Opus music, `video/` the MP4 videos (`logos.mp4`, `intro.mp4`, `intro2.mp4`, `ending.mp4`), `vib/` the HD Rumble files, `glsl/compiled/` the shaders.
- The save holds `monsterboy0.dat` and `system.dat`.

## Shaders

- **Current:** 202 loose files in `glsl/compiled/`, `PGRM_<name>.glslcobj`, one program each: a vertex and a pixel shader. 398 distinct shaders (199 vertex, 199 pixel), all translated for the Mac. Read by `glslcobj_shaders`: [Game Atelier's .glslcobj programs](../../../docs/reference/shaders.md#game-ateliers-glslcobj-programs).
- **Resolved: a white logo video, then a black screen.** The first reader assumed 16-byte alignment for the stage records and skipped the 98 programs whose names end elsewhere; the game asked for 6 shaders that were not in the map (`[RED] shader … is not in the shader map`, `program has no vertex shader`), among them the ones the videos draw with. The records are 8-byte aligned.
- The prewarm list (`pso_manifest.tsv`) is empty.

## Memory

- The footprint was about 1.9 GB during gameplay.

## Found on this game

- **Game Atelier's .glslcobj programs** (`glslcobj_shaders`): [shaders](../../../docs/reference/shaders.md#game-ateliers-glslcobj-programs).

## Open

- Only update 1.0.5 (v327680) was built; the base game was not checked.
- iOS was not checked.
