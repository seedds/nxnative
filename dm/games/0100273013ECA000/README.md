# Summer Pockets REFLECTION BLUE (`0100273013ECA000`)

Key / Visual Arts visual novel, SDK 10.8.2, update 1.0.1 (v65536). Luca System engine on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie 10.8.2), and `sdk` (nnSdk 10.8.2). Shares engine architecture and host requirements with Stella of The End (`01007200201C8000`). Plays on the Mac via `--platform mac-app`: opening movie, mini games, saving/loading, backlog, text skipping, and menus all run cleanly at 60 fps with sound.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0xe0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | 2816 MB heap region matching the NPDM memory pool and Luca System requirements |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource (virtual address memory is disabled) |
| `romfs_marker BGCG.PAK` | `game.tsv` | root RomFS marker file |
| `zip_store .pak:.webm:.metallib` | `game.tsv` | store uncompressed video and pak resources |

No custom rewriter patches (`patches.py`) required.

## Files and shaders

- RomFS: 43 files (38 `.pak` archives at root including `BGCG.PAK`, `CHARCG.PAK`, `FONT.PAK`, etc., and 5 `.webm` files in `/movie/`).
- Shaders: 50 BNSH shaders inside `/SHADER.PAK` (39 pixel shaders, 11 vertex shaders), translated into Metal libraries via `dmsh_build.py`.

## Mac gameplay

- Tested on Apple Silicon Mac (M4 Max) using the standalone Mac App bundle (`Summer Pockets REFLECTION BLUE.app`).
- Tested features: Opening movie (`.webm` playback via libvpx), mini games, save/load, backlog rewind, skipping, dialogue fonts, and title menus.
- Log report across >47,000 presented frames (~17.5 minutes of continuous play):
  - 0 `[RED]` errors
  - 0 shader compile errors or missing variations
  - 0 audio buffer underruns across >210,000 audio device callbacks
  - Steady 60 fps rendering with MetalFX spatial upscaling (1280x720 → native Retina display)
  - Memory resident set settled stably around 700 MB – 2.6 GB total footprint

## Found on this game

Nothing new: the reverse `DT_INIT` order, `MediaData` frame-rate division fix, linear render targets, and `ALL_GRAPHICS_BITS` pipeline handling previously introduced for Stella of The End covered this title completely.

## Open

- Not tested on iOS.
- Base game v0 build registration (currently registered on v65536 / 1.0.1).
