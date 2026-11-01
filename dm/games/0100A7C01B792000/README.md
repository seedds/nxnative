# Minecraft Dungeons II (`0100A7C01B792000`)

Update 1.1.1.0 (v65536) over the base game; SDK 20.5.15. Unreal Engine 5 on its mobile renderer, NVN, Wwise sound, Havok, Xbox Live services (XSAPI / libHttpClient). Modules `main`, `subsdk0` (the movie library) and `sdk`. Plays on the Mac at 60 fps (30 on the Switch) by the host's writes of Unreal's frame limits.

Status: brought up on the Mac and played (M1 MacBook Air, 8 GB, macOS 26, 1080p): menu, the opening level (Squid Coast) and the inventory, no crash in the sessions run; frame time p50 16.6 ms. The level's campfire, the hero and pickups draw right before and after the inventory ([Shaders and pipelines](#shaders-and-pipelines)). No `patches.py`. Not brought up on the iPhone ([Memory](#memory)). Played offline: Xbox Live sign-in is not available.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `fps_target 60` | [`game.tsv`](game.tsv), `hle_fpslock.c` | the host writes 60 into `t.MaxFPS` and 1 into `rhi.SyncInterval`, which the rewriter finds by name; see [Frame rate](#frame-rate) |
| `r.BloomQuality` 0 | `hle_fpslock.c` (`k_cvars`, `main` `9e9d887d…`) | a workaround: bloom's last passes drew blocks of other content in some runs, cause not found; see [Open](#open) |
| 64 GB alias region: `mem_reserve 0x1028000000`, `mem_alias 0x1000000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | Unreal's `nninitStartup` reserves a 62 GB address region and its FMallocBinned3 takes 37 GB of it (as [Octopath Traveler 0](../01005270232F2000/README.md#memory)); Mac only |
| `tex_dyn_slots on` | `game.tsv` | Niagara particle vertex shaders (`d1c8e612ec7462b6`, `210d8abb3b9996c5`: the campfire's flame) read a buffer texture whose handle the translator cannot trace to one slot |

## Frame rate

- **Current:** 60 fps. `t.MaxFPS` (float, registered at `main+0x33eaa90`, object at `main+0xae22500` in 1.1.1.0) and `rhi.SyncInterval` (int, `main+0x147dc3c`, object at `main+0xac91a10`) take the host's writes (`hle_fpslock.c`, profile key `fps_target 60`); the rewriter finds them by name ([Unreal's frame limit and present interval, found by name](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name)).
- Unreal's Viewport Thread waits for presented frames in the layer's frame history (`nn::vi::ListFrameHistory`); see [Found on this game](#found-on-this-game).
- In a 4-minute session on the M1 Air: p50 16.6 ms, 4 of 51 windows with p95 above 20 ms, 9 with one frame above 100 ms (pipeline compiles of draws met for the first time).

## Memory

- Unreal's `nninitStartup` reserves a 62 GB region and its FMallocBinned3 constructor 37 GB of it. The profile gives the Mac a 64 GB alias region and keeps the game's allocator. An iPhone needs the allocator cut for this build and a capped region (`game_args`), as Octopath Traveler 0's profile ([docs/reference/memory.md](../../../docs/reference/memory.md)); not done.

## Shaders and pipelines

- 15,826 shaders in the two shader code archives (`shader_groups`: `global` and `game`): 3,481 vertex, 10,354 pixel, 1,987 compute and 4 geometry.
- **Resolved: noise and black flicker on the hero, the campfire and pickups after the inventory.** The inventory's hero preview specializes the hero's Metal 4 base for its own target formats, which broke the scene's pipelines made from the same base object. Each colour format set now gets a base object of its own: [Metal 4 bases shared across colour formats](../../../docs/reference/shaders.md#metal-4-bases-shared-across-colour-formats).
- `31f5c2a62e017a2d` writes a `float4` into an R8Uint target; the pipeline is refused, as on the MTLDevice path ([Integer colour attachments on Metal 4 bases](../../../docs/reference/shaders.md#integer-colour-attachments-on-metal-4-bases)).
- **Resolved: frames held up in submit while walking** (25 ms, 47 fps windows): [Buffer texture views made again every frame](../../../docs/reference/rendering.md#buffer-texture-views-made-again-every-frame).

## Mac routes

- `scripts/build_ipa_from_nsp.py` on the NSP folder (base, update) packs and builds the Mac host; the build folder's `start_mac.sh` starts it.

## Found on this game

General host changes first needed here; each applies to every game:

- [An ssl context offline](../../../docs/reference/services.md#an-ssl-context-offline): `XblInitialize failed: 0x80070057` at boot.
- [Layer frame history](../../../docs/reference/services.md#layer-frame-history): the boot stopped for good right after the intro movie.
- [Buffer texture views made again every frame](../../../docs/reference/rendering.md#buffer-texture-views-made-again-every-frame).
- [Metal 4 bases shared across colour formats](../../../docs/reference/shaders.md#metal-4-bases-shared-across-colour-formats).
- [Integer colour attachments on Metal 4 bases](../../../docs/reference/shaders.md#integer-colour-attachments-on-metal-4-bases).

## Open

- Bloom is off (`r.BloomQuality` 0, a workaround): with it on, its last blur passes (30x34 and 60x34, fragment shaders `762a3933d873c16b` and `37c206…`) came out in some runs as blocks of other content from the title screen on: orange and cream rectangles over the whole picture and the HUD, the campfire green. Their inputs, shaders, draw state and load / store choices were the same as in runs without it; the cause is not found. Whether the [Metal 4 base fix](../../../docs/reference/shaders.md#metal-4-bases-shared-across-colour-formats) also removes it is not checked.
- `31f5c2a62e017a2d`'s fact row (`{"fout":{"0":"uint"}}` in a `shader_facts_manual.tsv`) is not added; it needs a shader rebuild.
- No `smoke.tsv` route yet, so `regress.py` checks only the chain on this game.
- iOS ([Memory](#memory)), Xbox Live sign-in, online play and long sessions are not tested.
