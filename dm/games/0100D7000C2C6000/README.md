# Katamari Damacy REROLL (`0100D7000C2C6000`)

Update 1.2 (`v131072`), SDK 5.4.0, on NVN. Unity 2018.1.2f1 (IL2CPP 64-bit ARM), CRIWARE (USM movie playback), and FMOD. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. 7,212 NVN shaders in UnityFS bundles.

The standard host and rewriter run this build with no game-specific fixes: no shader overrides, rewriter cuts or custom host hooks required. Plays on the Mac at 1080p 60 fps with audio, full controller support, and opening movie playback.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `main` imports movie playback from `subsdk0` and standard runtime services from `sdk` |
| 4 GB physical layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | `main` sets a heap of at most pool − 64 MB (3328 MB heap region). The rewritten modules use 0x6600000 (102 MB) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, standard physical memory |
| `shader_groups /Data/=data` | `game.tsv` | Unity shaders reside in `Data/StreamingAssets` UnityFS bundles |
| `zip_store .assets:.dat:.resource:.ress:.usm:.metallib` | `game.tsv` | uncompressed storage in the app package for movie streams and large Unity asset packs |

## Shaders

- 7,212 NVN shaders extracted from UnityFS archives via `unityfs.py` and translated to Apple Metal.
- Shader map contains 7,212 entries with 0 conflicts.

## Rendering & Gameplay (Mac)

- Verified on Apple Silicon (M1 / M4): opening cutscenes play cleanly through CRI USM decoder, main menu renders in 1080p widescreen, and gameplay holds locked 60 fps.
- Zero unresolved HLE calls or missing symbols on SDK 5.4.0.
