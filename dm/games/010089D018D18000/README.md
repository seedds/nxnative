# We Love Katamari REROLL+ Royal Reverie (`010089D018D18000`)

Update 1.0.3 (`v131072`), SDK 14.3.1, on NVN. Unity 2021.3.16f1 (IL2CPP 64-bit ARM), CRIWARE (USM movie playback), and FMOD. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. 8,245 NVN shaders in UnityFS bundles.

Plays on the Mac at 1080p 60 fps with audio, controller support, and opening movie playback.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `main` imports movie playback from `subsdk0` and standard runtime services from `sdk` |
| Virtual address memory: `sys_resource 0x1000000`, `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | `game.tsv` | the NPDM declares 16 MB of system resource (`IsVirtualAddressMemoryEnabled` is true). `nninitStartup` hands application pool − used memory to StandardAllocator in the alias region. The pool is reported as 1 GB (`0x40000000`) to prevent virtual address space exhaustion |
| AddressRegionManager node area cut (`mov w8, #0x3fe00000` → `mov w8, #0x3e00000`) | `dm/tools/sdk_patches.py` | SDK 14's AddressRegionManager allocates 1 GB of the alias region as its node area; the cut shrinks it to 62 MB, as for Fire Emblem Engage |
| `shader_groups /Data/=data` | `game.tsv` | Unity shaders reside in `Data/StreamingAssets` UnityFS bundles |
| `zip_store .assets:.bundle:.dat:.json:.ress:.metallib` | `game.tsv` | uncompressed storage in the app package for movie streams and large Unity asset packs |

## Shaders

- 8,245 NVN shaders extracted from UnityFS archives via `unityfs.py` and translated to Apple Metal.
- Shader map contains 8,245 entries with 0 conflicts.

## Rendering & Gameplay (Mac)

- Verified on Apple Silicon (M1 / M4): opening cutscenes play cleanly through CRI USM decoder, main meadow renders in 1080p, and gameplay holds locked 60 fps with zero audio underruns.
- Zero unresolved HLE calls or missing symbols on SDK 14.3.1.
