# TRIANGLE STRATEGY (`0100CC80140F8000`)

Update 1.1.1 (v262144) over the base game, SDK 12.3.2 (the same `sdk` and movie builds as OCTOPATH TRAVELER II). Square Enix / Artdink, Unreal Engine 4.27.0 (`SDK MW+EpicGames+UnrealEngine-4_27_0`): everything in one pak (`Newera/Content/Paks/Newera-Switch.pak`, 5.85 GB), NVN, PhysX, CRIWARE sound (a 6-channel AudioOut beside the audio renderer), the title's background movies through the SDK's movie module. Modules `main`, `subsdk0` (the movie module) and `sdk`. 60 fps (30 on the Switch) by the host's write of Unreal's present interval.

Status: brought up on the Mac on 2026-11-02: the update notice, the title and its background movies, a new game, the narrated opening, the prologue's scenes and world map, the first battle (Wolffort Harbor: moving the camera, attacks, a fire spell, enemy turns, level ups, its tutorials), the quicksave and its load. At a steady 60 fps (frame time p50 16.7 ms, p95 about 17 ms) with sound. The battle's last enemy was not reached, so the end of a battle and everything after it are untested. No IPA has been built and nothing has run on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game sets near = 0 at startup (log: `depth mode near=0`), so the pipeline bases made before it runs take the `main0_z` vertex entries |
| `fps_target 60` | `game.tsv`, `hle_fpslock.c` | the host writes 1 into `rhi.SyncInterval`, which the game sets to 2; see [Frame rate](#frame-rate) |
| FMallocBinned3 constructor call cut, with gate `OBS_UE_MALLOC` | the rewriter's engine-wide rule (`dm/tools/ue_malloc.py`), no build ID | the host's allocator runs Unreal's heap (`hle_uemalloc.c`); see [Memory](#memory) |
| Engine region given what the alias region has room for, 2.81 GB: gate `OBS_UE_REGION` on the store of its size, `main+0xd18fd0`, and `main+0xd18fdc` made `mov x1, x8` | the rewriter's engine-wide rule (`dm/tools/ue_region.py`), no build ID; the host's room (`hle_svc.c`) | Unreal's `nninitStartup` reserves 62 GB by default; see [Memory](#memory) |
| Node area cut 1 GB → 62 MB, regions aligned to 2 MB | `dm/tools/sdk_patches.py`, the SDK-wide rules on `sdk` build `5bcf525f…` | the same `sdk` as OCTOPATH TRAVELER II |
| 3.75 GB guest block: `mem_reserve 0xF0000000`, `mem_alias 0xC8000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the layout of the other Unreal 4.27 games: alias region = node area + the engine region + 256 MB for regions made later |
| `sys_resource 0x5000000` | `game.tsv` | the NPDM declares 80 MB of system resource |
| `pad_layout full` | `game.tsv` | the 3D layout: the right stick turns the battle camera. It plays with the controller, so it stays docked (1920x1080) |
| `shader_groups ShaderArchive-Global-=global ShaderArchive-=game` | `game.tsv` | the two shader code archives: the engine's global shaders and the game's material shaders |

## Frame rate

- **Current:** 60 fps (30 on the Switch). The game sets `rhi.SyncInterval` to 2; the host writes 1 whenever it does (`main: Unreal rhi.SyncInterval … 2 → 1 vsyncs`).
- The same scripted route at 30 and at 60 fps (`run/ab.sh`, frame time p50 33.3 → 16.7 ms) showed the opening narration's map at the same stage in shots at 70, 80, 90 and 100 s: the game steps by the time that really passed. Battles were played at 60 fps but not compared with 30. `DM_FPS_TARGET=30` keeps the game's 30.

## Memory

- **Current: a 2.81 GB engine region in a 3.75 GB block.** On the Mac, through the title, the prologue and the first battle: the alias region mapped up to +1.25 GB of 3.12 GB, the Unreal heap up to 1.00 GB, 3.75 GB resident with 1.86 GB of Metal allocations.
- **Open: a step of about 730 MB of Metal allocations in the battle.** Around frame 67,000 of one run, per-frame arena blocks went from 40 to 169 in a stretch with many geometry draws, taking the run to 4.3–4.5 GB resident and 2.6 GB of Metal allocations. It then stayed flat for the 33,000 frames to the run's end, so it is no leak, but what asked for it is not known, and it may matter on the iPhone.
- **Resolved: an abort at boot in the 3.75 GB block** (`nn::diag` abort from `main+0xd0f130`, just after the NVN device starts), while the provisional 64 GB alias region booted. The rewriter did not recognise this build's reservation, where x1 is set from the constant after the store of the size, so the engine asked for its whole 62 GB. The rewrite said so in a `[RED]` line: [Unreal's engine address region and its options](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options).

## Files and shaders

- The update (1.23 GB) is a delta over the base RomFS, so the merged extraction holds the whole game. The two shader code archives are in the pak (`ShaderArchive-Global-SF_GLSL_SWITCH.ushaderbytecode`, 29 MB, and `ShaderArchive-Newera-SF_GLSL_SWITCH.ushaderbytecode`, 693 MB decoded), each shader LZ4-compressed, so `uepak.py` decodes them and no host run is needed.
- Shaders: 25,887 unique (4,017 vertex, 21,108 pixel, 694 compute, 68 geometry). The game has no PC version in this repository, so the facts table holds only compute group sizes.
- Prewarm list ([`pso_manifest.tsv`](pso_manifest.tsv), 2,439 lines): recorded on the Mac with prewarm off over the boot, the title and its movies, a new game, the prologue into the first battle, turns of that battle with attacks, and a load of its quicksave. From an empty save with it and `DM_PSO_PREPARE=1`, the route to that battle's opening dialogue built nothing on demand (an empty `pso_user.tsv`). The prewarm counts its 8 geometry variant lines (`_bg3`, `_bgv`) as not in the shader map, though their base shaders are there; OCTOPATH TRAVELER II's list has such lines too.

## Input

- The update notice on a fresh save closed with B and not with A. A turns a help page and B closes it; on the first battle's move tutorial B is greyed out on its first page. Plus opens the skip prompt in scenes that can be skipped (the battle's opening dialogue cannot). A dialogue choice shows no cursor at first: press a direction, then A.
- A battle's command menu: A on a unit's square opens it; with the cursor on another square A gives `Simulation`, a preview in which nothing can be issued, and B twice returns to the unit.

## Mac routes

The work folder's `run/` has `host.sh` (copies a host to `build/tshost`), `boot.sh <log> [seconds] VAR=…`, `seq.sh` / `see.sh` / `mash.sh` (presses and shots), `stop.sh`, `to_battle.sh` (blind route from an empty save to the first battle's opening dialogue, about 9 minutes), `to_load.sh` (from a save with a quicksave back into that battle), `fight.sh` (attack and end-turn cycles), `ab.sh` (the frame-rate comparison) and `rec.sh` (recording the prewarm list). `run/save_ref` holds a quicksave in the first battle. There is no [`smoke.tsv`](../README.md) yet.

## Found on this game

General changes first needed here; each applies to every game:

- **The engine region's size set again after its store** (`dm/tools/ue_region.py`, `patch_nso.py`): [Unreal's engine address region and its options](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options).
- **Movies into BGRA8 client textures** (`hle_movie.c`, `md_out_tex`): [A BGRA8 client texture gets its movie with red and blue swapped](../../../docs/reference/movies.md#a-bgra8-client-texture-gets-its-movie-with-red-and-blue-swapped). The title's great hall had blue torches and a blue floor circle before it, warm ones after.
