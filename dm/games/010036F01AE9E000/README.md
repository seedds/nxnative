# Pentiment (`010036F01AE9E000`)

Update 1.3.2 (v131072) over the base game, SDK 16.2.0, NVN. Obsidian Entertainment's narrative adventure on Unity 2021.3.23f1 IL2CPP with FMOD; modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The standard host and rewriter run this build with no game-specific cut or host change. The profile is the NSP script's provisional one with its names, category, and the heap and address-region checks below, confirmed in play.

On the Mac (2026-11-09, M1, 8 GB): the title screen, new-game profile selection, autosave notice, opening book and the stone interaction render and accept input at about 60 fps (frame time p50 16.4–16.6 ms; power reports 55–60 fps), with about 1.6 GB resident and no `[RED]` line or missing shader. The player's own play test found no flaw. The IPA is not built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout for this virtual-address-memory game; 15 s into `nnMain` its regions reached alias +4.04 GB of the 4.375 GB region |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM's 16 MB of system resource |
| `depth_mode zero` | `game.tsv` | the heap-check boot saw `nvnDeviceSetDepthMode` with near = 0 |
| `shader_groups /Data/=data` | `game.tsv` | the shaders are in the RomFS UnityFS bundles under `Data/` |

No code of the game is cut and no frame-rate key is set: the game asks for present interval 1 and runs at 60 fps. `pad_layout` keeps the default 2D layout.

## Files and shaders

- RomFS: 2,219 files, 4.24 GB. 1,755 UnityFS `.bundle` files hold 4.13 GB, mostly under `Data/StreamingAssets/aa/Switch/`; the rest are conversation, string-table and game-data bundles.
- 7,260 shaders: 3,630 vertex and 3,630 pixel, with the remaining compute and geometry shaders, split from `Data/` and from the Unity player's GLSLC outputs in `main`. All built for the Mac with no failure; no run met a shader missing from the map.
- The Mac pipeline archive is 80.6 MB. The prewarm seed (`pso_manifest.tsv`) is empty.

## On the Mac

- From the title: A opens the three profile slots; A on the first slot, then A through the autosave notice, enters the opening book. A operates the book's interactions and L moves the stone.
- The game draws 1920x1080. The opening's 300-frame reports were p50 16.4–16.6 ms and GPU p50 8.4–8.6 ms.
- `DM_CTL` can drive it after the host starts; the title and menus accept A and the left stick as expected.

## Memory

- 1.6 GB resident and 812 MB of Metal allocations in the opening book, with 27 live memory pools.
- The address-region check reached alias +4.04 GB in the first 15 s, leaving about 0.34 GB of the profile's 4.375 GB region; what later chapters map is not measured.

## Open

- iPhone: no IPA built.
- The agent's test covered the title, new-game flow, opening book and stone interaction; the player's wider test found no flaw, but save reload and the other languages were not separately checked.
- The prewarm list is empty.
- The address-region headroom above.
