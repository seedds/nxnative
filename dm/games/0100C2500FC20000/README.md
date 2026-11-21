# Splatoon 3 (`0100C2500FC20000`)

Update 12.0.0 (v2818048) over the base game, SDK 20.5.32, Nintendo EPD's sead / agl / gsys engine on NVN, with Pia and NPLN for online play. The ExeFS loads `main` and `sdk`. My build packed my three DLC packages. This is a provisional Mac profile; online play stays off, as everywhere in this repository.

| Module | NSO build ID |
|---|---|
| `main` 12.0.0 | `6cbb16698653e1af3c3a7602164819bb` |
| `sdk` 12.0.0 | `6a0a2600867712cd6f6c4dec53ae0d9c` |

## Profile

- **Memory.** The application layout comes from the executable's declared requirements (`mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`). The NPDM declares no system resource (`sys_resource 0x0`), and the rewritten modules need `mem_used 0x7600000`.
- **Shaders.** The tables, made from 12.0.0's files, describe 31,224 unique shaders: 13,020 vertex, 17,984 pixel, 100 compute and 120 geometry. There is no `validated.tsv`; I do not claim that every shader is exercised.
- **No game-specific cuts or patches.** `romfs_marker Model/Fld_PlayerMake.bfres.zs` identifies the resource root.

## Rendering

- **Resolved: blown-out lobby.** In my NX Port Studio builds of the same game, the plaza showed black dots and coloured halos, and the lobby and its practice area came out blown out in black and white. Frame captures found NaN in the HDR scene target, about 690,000 pixels in the lobby. I traced it to the environment-probe array's initial content: 35 % of its texels were NaN left in reused storage. The Switch's blender discards them under a zero weight; Apple's does not. With [stale floating-point storage at first use](../../../docs/reference/nvn-objects.md#stale-floating-point-storage-at-first-use), my replay of the probe-generation frames went from 1,790,418 NaN texels to none, and the plaza, lobby and practice area rendered as on my Switch. It works together with [a NaN colour under a zero blend weight](../../../docs/reference/shaders.md#a-nan-colour-under-a-zero-blend-weight).
- **Open: eager pipeline prebuild.** On my M5 Max the first launch's prebuild of 27,707 pairs lost pairs to Metal compiler-service interruptions (`XPC_ERROR_CONNECTION_INTERRUPTED`), and the service eventually stopped answering. `DM_PSO_PREBUILD=0` leaves the pipelines to the draws.

## Mac testing

On 2026-11-21 I built 12.0.0 for the Mac from this branch (`upstream/main` `e74c3fa7` plus this change) on an M5 Max (macOS 27.2) with `--gpu none`. All 31,224 shaders compiled. With `DM_PSO_PREBUILD=0` the game loaded my save and reached Splatsville's plaza, and I walked around it with the controller.

- **Plaza.** Before the per-texel part of [stale floating-point storage at first use](../../../docs/reference/nvn-objects.md#stale-floating-point-storage-at-first-use), the buildings' reflections showed black and red blocks after a return from the lobby. A dump of the 128×128×78 probe array (`DM_CTL` `texraw`) found 14 NaN texels (0.001 %), under the 1 % threshold, so its storage had been kept. With the fix the plaza rendered as on my Switch, also after the same return. In that run the lobby attempt and the return gave six textures a few non-finite elements at first use, from 79 to 22,215 (0.32 %, in a 256×256×78 probe array), and one probe array started from zeros; all were in `CPU_NO_ACCESS` pools.
- **Lobby.** Entering it stops at a connection error, as expected with online play off; the game then returns to the plaza.
- **Focus.** A window behind other windows takes the game out of focus (`oe focus state → 2`), and the game then draws only its HUD until the window is in front again. With `DM_FOCUS_KEEP=1` the title screen and the plaza kept drawing with Finder in front.
- The log's only red lines are one texture-type mismatch (a 3D texture bound where a shader declares a 2D one) and one invalid colour clear target.

## Found on this game

- [Stale floating-point storage at first use](../../../docs/reference/nvn-objects.md#stale-floating-point-storage-at-first-use).

## Open

- Sustained play, the DLC's content and iOS.
- The prebuild's compiler-service interruptions.
