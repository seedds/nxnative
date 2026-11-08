# Stella of The End (`01007200201C8000`)

Key / Visual Arts visual novel, SDK 18.3.2, base game 1.0.0 (v0). Luca System engine on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie 18.3.2), and `sdk` (nnSdk 18.3.2). Plays on the Mac via `--platform mac-app` and on iOS.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0xe0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | 2816 MB heap region matching the NPDM memory pool and Luca System requirements |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource (virtual address memory is disabled) |
| `romfs_marker image/BGCG.PAK` | `game.tsv` | RomFS marker file |
| `zip_store .pak:.webm:.metallib` | `game.tsv` | store uncompressed video and pak resources |

No custom rewriter patches (`patches.py`) required.

## Files and shaders

- RomFS: 35 files (33 `.pak` archives including `image/BGCG.PAK`, and 2 `.webm` files in `/movie/`).
- Shaders: 55 BNSH shaders inside `/SHADER.PAK`, translated into Metal libraries via `dmsh_build.py`.

## Found on this game

- `loader.c`: Run `DT_INIT` in reverse load-list order matching Horizon `rtld`.
- `hle_movie.c`: Populate `MediaData` frame-rate to avoid division by zero during video playback.
- `hle_nvn.c`: Support linear layout for `LINEAR_RENDER_TARGET` in `write_texels`.
- `nvn_mtl.m`: Handle BNSH programs initialized with `ALL_GRAPHICS_BITS`.

## Open

- Additional iOS validation.
