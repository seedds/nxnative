# Overcooked! All You Can Eat (`0100F28011892000`)

Update 1.0.11 (v720896) over the base game, SDK 10.4.1. Unity 2019.4.11f1 IL2CPP with FMOD and Pia on NVN. Plays on the Mac. Tested there in a 35-minute run (63,000 frames): the menus, the Winter Wonderland and Night of the Hangry Horde world maps and a level on each, with music and no `[RED]` line; then 4.5 minutes at 60 fps. iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `present_interval display` | [`game.tsv`](game.tsv) | 60 fps: the game asks for interval 2 and nothing else holds it at 30; see [Frame rate](#frame-rate) |
| `depth_mode zero` | `game.tsv` | the game selects near-zero NVN depth; offline pipeline bases must use the matching vertex entry |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NSP script's provisional values, checked by its address region check; see [Memory](#memory) |
| `unity_tier 2` | `game.tsv` | the player runs one hardware tier and Unity shipped all three ([Shaders](#shaders)) |
| `unity_skip_keywords DIRLIGHTMAP_SEPARATE DIRLIGHTMAP_COMBINED DYNAMICLIGHTMAP_ON POINT_COOKIE DIRECTIONAL_COOKIE` | `game.tsv` | keyword variants the game data never enables ([Shaders](#shaders)) |
| `unity_skip_passes META` | `game.tsv` | the lightmapper's pass, which only the editor draws ([Shaders](#shaders)) |
| `shader_groups /Data/=data` | `game.tsv` | everything is under `Data/`: the player's serialized files and the Addressables bundles in `StreamingAssets/aa/Switch` |

## Memory

- The heap check found no heap set; the address region check (the game's first 15 s in `nnMain`) saw its regions reach alias +3.38 GB, inside the 4.375 GB alias region.
- Measured with the host's `[mem]` lines on the Mac: the alias region mapped up to 2.76 GB of the 3.21 GB application pool after 35 minutes. Footprint 3.83 GB, flat over the last minutes of the run.

## Shaders

- RomFS: 575 files, 12.4 GB, 557 of them UnityFS bundles (Addressables, `Data/StreamingAssets/aa/Switch`).
- Unity shipped every tier and variant: 520,394 stage shaders, a 2.3 GB package and 293k pipeline pairs. The profile keys ([Unity programs for every hardware tier](../../../docs/reference/shaders.md#unity-programs-for-every-hardware-tier), [Unity variants and passes the player never draws](../../../docs/reference/shaders.md#unity-variants-and-passes-the-player-never-draws)) leave 45,221 (25,221 vertex, 19,976 pixel, 3 geometry, 21 compute), a 198 MB package and 28,928 pipeline pairs:

  | Key | Left out | Evidence | Shaders after |
  |---|---|---|---|
  | `unity_tier 2` | tiers 0 and 1 | every shader a run loaded was tier 2; no key is shared between tiers | 173,454 |
  | `DIRLIGHTMAP_SEPARATE` | separate-directional lightmaps | no `LightmapSettings` in mode 2 | |
  | `DYNAMICLIGHTMAP_ON`, `DIRLIGHTMAP_COMBINED` | realtime and directional lightmaps | the baked lightmaps are non-directional; 8 of about 214,000 renderers have a realtime lightmap | 56,091 |
  | `POINT_COOKIE`, `DIRECTIONAL_COOKIE` | light cookies | none of the 10,369 lights has a cookie | 47,961 |
  | `unity_skip_passes META` | the lightmapper's pass | drawn only by the editor | 45,221 |

- **Limits:** the 8 realtime-lightmap renderers are on two DLC world maps (Winter Wonderland and Night of the Hangry Horde), where play drew only `DYNAMICLIGHTMAP_OFF` variants. `Light.set_cookie` is in the IL2CPP metadata, so a script could give a light a cookie. A left-out variant the game asks for logs `failed to load shader`.
- **Resolved: offline depth metadata.** Without `depth_mode zero` the bundled archive was made for near = −w and rejected, and the first boot built 34,444 pipeline pairs itself (227 s, with audio underruns). With it, all 305 bases came from the archive, 1.5 s before the first frame.

## Frame rate

- **Current:** 60 fps (30 on the Switch). The game asks for present interval 2 (Unity's 30 fps setting) and nothing else holds it at 30: `present_interval display` presents a frame every display tick. On an M1 Pro (macOS 26.7.2), over 16,200 frames: 59.9–60.0 fps, frame time p50 16.7 ms, p95 16.9–20.5 ms; GPU p50 up to 8.8 ms, p95 up to 16 ms; CPU energy about 0.86 W.
- `DM_PRESENT_INTERVAL=game` keeps 30 and saves power: frame time p50 33.4 ms, p95 36.5 ms; GPU p50 7–9 ms, p95 11–12 ms; CPU energy about 0.15 W.

## Audio

- FMOD; music and effects play. No underrun after the first frame, at 30 or 60 fps.

## Open

- iOS untested: the 3.83 GB Mac footprint is above what an iPhone gives an app without the increased memory limit entitlement.
- The left-out lightmap and cookie variants ([Shaders](#shaders)): not every level was played.
- Game speed at 60 fps: it looked right in play but was not timed against a clock.
- Online play (Pia, NEX) is not tried.
- The base game alone (v0) is not registered.

## Found on this game

- [Unity programs for every hardware tier](../../../docs/reference/shaders.md#unity-programs-for-every-hardware-tier) (`unity_tier`).
- [Unity variants and passes the player never draws](../../../docs/reference/shaders.md#unity-variants-and-passes-the-player-never-draws) (`unity_skip_keywords`, `unity_skip_passes`).
