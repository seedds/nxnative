# Pokémon Mystery Dungeon: Rescue Team DX (`01003D200BAA2000`)

Update 1.0.2 (v131072) over the base game, SDK 7.4.0. Unity 2018.3.12f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. No host change was needed: the profile is the NSP script's provisional one, kept as guessed. Plays on the iPad (see [Testing](#testing)); not yet run on the Mac.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout with the hardware pool: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | Engage's block (256 MB in front + 4.375 GB alias region + 256 MB stack region + 128 MB heap region); see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game selects near-zero NVN depth; offline pipeline bases must use the matching vertex entry |
| `shader_groups /Data/=data` | `game.tsv` | |
| No `pad_layout` (the 2D default) | `game.tsv` | the profile guess; the game was played with a controller |

## Memory

- The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true: `nninitStartup` hands the application pool to StandardAllocator as one address region in the alias region (`[mem] address region 3.14 GB at alias +0.06 GB, returning to main+0x1655200`). The NSP script's heap check found no heap set.
- As in [Raging Loop](../010020400E1C2000/README.md#memory) (Unity 2018.3), the game's regions end at alias +3.21 GB of 4.38 GB, so the hardware pool stays. The heap check measured only the first 15 s in `nnMain`; what the game maps later in play is not measured.

## Files and shaders

- The RomFS holds 3,363 files, 2.05 GB: 2,670 Unity bundles (`.ab`, `UnityFS`) in `Data/StreamingAssets` take 1.92 GB, with 391 Lua scripts.
- 9,817 shaders, split from the bundles' Unity `Shader` objects and `main`'s GLSLC outputs. `pso_manifest.tsv` is empty: no prewarm list has been recorded.

## Testing

- **iPad Pro 11" (M1, iPad13,4), iPadOS 26.5, 2026-11-08.** An app built with Xcode from this profile (chain `39417042cfe8`, host 7f50222003e4), handheld (`DM_OPMODE=0`), an Xbox Wireless Controller as player 1. The player reported it running smoothly. The log of one run, about 5.5 minutes, has no `[RED]` lines; the game holds 30 fps, its rate on the Switch, with frames of 33.3 ms and about 19–20 ms of GPU time in play, 814 MB in the residency set, CPU energy about 0.2 W.
- The first frame comes 25 s after launch. With no pipeline archive (the profile has no `pso_mtl4`), the third frame built 715 render pipelines in 4.1 s, and the screen stays black until then.

## Found on this game

Nothing new: the provisional profile from `dm/games/profile_guess.py` played as guessed.

## Open

- Not yet run on the Mac, so it is not in the README gallery.
- No prewarm list (`pso_manifest.tsv`) or pipeline archive: the first start builds its pipelines on the device.
- Only the first minutes were played on the iPad; later dungeons, the movies (`subsdk0`) and docked mode are unchecked.
