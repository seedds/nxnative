# Yoshi and the Mysterious Book (`0400245021D2C000`)

A Switch 2 game: update 1.0.2 (v131072), SDK 20.5.15 built for the Switch 2 (`Ounce`). Unreal Engine 5.4.4 (`SDK MW+EpicGames+UnrealEngine-5_4_4` in `main`) with its NVN2 RHI. Modules `main`, `subsdk2` (NintendoSDK_movie) and `sdk`; `subsdk0` (DLSS) and `subsdk1` (CUDA) are not loaded, as `main` imports nothing from them.

Status: **in bring-up.** On the Mac (M5, 24 GB), in a window, with a partial shader library of the keys in [`shader_seen.tsv`](shader_seen.tsv): the opening matches a Switch 2 reference picture through the Mr. E scene, and the first level and part of the second were played and work (reported by the player on chain 4a0c646a21a8). The rest of the game is untested, and no IPA has been tested. Not yet in the README's gallery.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x550000000`, `mem_alias 0x400000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Splatoon Raiders' Switch 2 split as a starting point. The engine region gets 15.69 GB at alias +0.06 GB; about 0.26 GB of the alias region and a 0.75 GB Unreal heap are in use at the title. Not measured in play |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB of system resource |
| `pad_layout full` | `game.tsv` | a default for a 3D game; not tested in play |
| `shader_groups GlobalShaderCache=global #Engine/=engine #pj045/=game` | `game.tsv` | the shaders' sources; see [Shaders](#shaders) |
| FMallocBinned3 constructor call cut at `main+0x11aa0`, gate on it | the rewriter's engine-wide rule (`dm/tools/ue_malloc.py`), no build ID | constructor `main+0xa0200`, getter slot 22: the UE 5 layout holds on 5.4 so far |
| Gate on the engine region's size, `main+0x11a80` | the rewriter's engine-wide rule (`dm/tools/ue_region.py`), no build ID | the reservation at `main+0x11a84` in `nninitStartup` (`main+0x11600`) |
| Node area cut 1 GB → 62 MB (`sdk+0x14df18` / `+0x14dfa4`) | `dm/tools/sdk_patches.py` SDK-wide rule | without it the reservation failed and the game called `exit(1)` (`main+0x11ad8`) |

## Files

- One [NUP container](../../../docs/development.md#the-packing-script-in-detail) of 19.5 GB with update v131072 and no base game; [`pack_nup.py`](../../../scripts/pack_nup.py) packs its update as the whole game. It carries an icon and no NACP, so the name comes from the profile.
- RomFS: 147 files, 19.42 GB. One IoStore container, `PJ045/Content/Paks/PJ045-Sage.utoc` / `.ucas` (TOC version 6, 18.5 GB), beside one pak, with 62 movies (`.webm`, `.mp4`).

## Shaders

- 133,154 in [`shader_map.tsv`](shader_map.tsv): 45,614 vertex, 60,840 pixel, 21,654 compute, 5,041 mesh, 5 geometry; split in about 50 s ([Unreal's Switch 2 shaders inline in cooked packages](../../../docs/reference/shaders.md#unreals-switch-2-shaders-inline-in-cooked-packages)). A full Mac library would take hours, so the runs here use a partial one: [`shader_seen.tsv`](shader_seen.tsv) lists what headless runs asked for ([Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders)).
- Current: a partial build of the 1,519 keys the runs asked for translates and compiles all of them. Each new area asks for more (about 60 to 400), so an area drawn black or with something missing may just lack its shaders: `failed to load shader <key>` in the log. Building them takes under a minute. A full build has not been tried.
- Current (open): `SURED` on 64 bits is refused ([the entry](../../../docs/reference/shaders.md#unreals-ampere-resource-forms-handles-in-the-games-uniform-slot)); 12,811 shaders have it, none asked for so far.

## Driving it on the Mac

Run with `DM_CTL` for presses and shots, `DM_WINDOW=1` to play. With a new save: `A` at the title after about 22 s, `A` again 8 s later, then after 6 s the movie takes **two** `Plus` to skip (send a few pairs, 8 s apart, for the ones that follow). Then `A` every 7 s or so goes through the bubbles. "Who's there?" has the game's picture behind it, never black: a black background there is a fault.

## Open

- The rest of the second level and the levels after it. An earlier run of the second level had odd behaviour and missing animations before its shaders were built; whether any of that remains is unknown.
- `tbuf_as_2d` remakes its view at every bind ([the entry](../../../docs/reference/shaders.md#1d-fetches-through-a-uniform-handle)); frame time in the first level is p50 16.8 / p95 39.5 ms.
- `[RED] the dispatched program has no compute shader` and `program has no vertex shader`, once a run; `geometry shader 502dcf1ce20751aa_b ... vertex shader -`, up to 8 lines on the second level: a draw with no vertex shader bound, cause not looked at.
- `[RED] image read as float ..., texture of Metal format 13`, once a run; `vertex attribute format` 8 / 16-bit integer feeding a float input, once a run.
- `nvnWindowBuilderGetPresentInterval` is not implemented: one red line at boot, with no effect seen.
- Memory in play, an IPA.

## Found on this game

- [More than 16 vertex attributes](../../../docs/reference/rendering.md#more-than-16-vertex-attributes): `BindVertexAttribState n=17`.
- [Unreal's Switch 2 shaders inline in cooked packages](../../../docs/reference/shaders.md#unreals-switch-2-shaders-inline-in-cooked-packages): no shaders found in the split.
- [Unreal's engine address region and its options](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options): `exit(1)` in `nninitStartup`, with the SDK's 1 GB node area.
- [Unreal's Ampere resource forms: handles in the game's uniform slot](../../../docs/reference/shaders.md#unreals-ampere-resource-forms-handles-in-the-games-uniform-slot): `0xf63`, and black passes.
- [1D fetches through a uniform handle](../../../docs/reference/shaders.md#1d-fetches-through-a-uniform-handle): vertex data read as zero.
- [A predicated LDG with its address in its destination](../../../docs/reference/shaders.md#a-predicated-ldg-with-its-address-in-its-destination): instances drawn at one place.
- [Both words of a type 1 handle](../../../docs/reference/shaders.md#both-words-of-a-type-1-handle): lighting without its G-buffer.
- [A guarded instruction's deferred write](../../../docs/reference/shaders.md#a-guarded-instructions-deferred-write): overexposure from the Mr. E scene on.
- [A BRX table followed by a word past the code](../../../docs/reference/shaders.md#a-brx-table-followed-by-a-word-past-the-code): two compute shaders.
- [Layer entry points of a vertex shader whose body is a function](../../../docs/reference/shaders.md#layer-entry-points-of-a-vertex-shader-whose-body-is-a-function): 38 vertex shaders.
