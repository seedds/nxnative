# Ori and the Will of the Wisps (`01008DD013200000`)

Update 1.2.1 (v196608) over the base game, SDK 9.4.0. Unity 2018.4.24f1 IL2CPP with Wwise, on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Brought up on the Mac on 2026-11-06 from the provisional profile: boot, the loading screen, the title, a new game and play in the prologue, with no shader missing from the shader map; checked again on main at bd979ced (2026-11-07), with the landed program-key fix. The game asks for present interval 1 (60 fps). Not tried on an iPhone; no IPA has been built, so its size is not measured.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Program keys that follow the program's serial (e5f03403, PR #1521) | `dm/gfx/nvn/hle_nvn.c` | Unity 2018.4 copies NVNprogram objects after `nvnProgramSetShaders` and draws with the copies; without it about 135 programs a boot are keyed from freed control sections and the screen is grey noise. See [Program keys computed inside `nvnProgramSetShaders`](../../../docs/reference/shaders.md#program-keys-computed-inside-nvnprogramsetshaders) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game sets the near = 0 depth mode, so pipeline bases made before it runs take its vertex shaders' `main0_z` entries |
| [`pso_manifest.tsv`](pso_manifest.tsv) | profile | the seed list; see [Pipelines](#pipelines) |
| the profile's other keys | `game.tsv` | the provisional profile's static guesses (`profile_guess.py`), unchanged: the NPDM declares no system resource, so the 4 GB layout with a 3.25 GB heap region; the game sets a 3.05 GB heap |

No `patches.py`, no `pad_layout`; `present_interval` stays `game`.

## Pipelines

- Unity hands the driver nearly every shader program at boot (14,769 shaders), so first-use shader loads are not the cost; the cost is the render pipelines built the first time an effect is drawn.
- Without a seed list, on a fresh save on the Mac (boot → title → new game → the prologue's opening), draws waited for pipelines 8.1 s in all over five minutes (up to 0.9 s per 300 frames), seen as stutters when an area or effect first appears; once built, the area holds 60 fps.
- The seed list is the `pso_user.tsv` of the bring-up runs, merged in first-use order: 382 render pipelines, 10 compute pipelines and the shaders. With it the prepare step takes 1.7–2.7 s before the first frame, and the same route had no draw wait at all, in five runs (three at bring-up, two on main at bd979ced).
- The list covers only the start of the game. Later areas still build their pipelines on first use, once per device (`pso_user.tsv`). To extend it, record a longer route: `DM_PSO_MANIFEST=off DM_PSO_RECORD=<file>` ([Prewarm lists](../../../docs/development.md#prewarm-lists)).
- The prebuild of every known shader pair (`shader_pairs.tsv` + `pso_pairs.tsv`, 26,600 pairs) takes about 2 minutes on an M4 Pro the first time and competes with the game meanwhile: the prologue ran at about 24 fps during it in one run, and at 50–57 fps with the archive built.

## Frame rate

The game sets present interval 1. On an M4 Pro Mac (macOS 26.7.2, window, main at bd979ced, archive built), two runs from a fresh save: the title and the prologue's opening hold 60 fps, with dips to 56.5–56.8 fps in a few 10-second windows, and a frame interval p99 of about 17.1 ms. On the older main the bring-up used (chain cfcc3a82c78b) the prologue ran at 50–57 fps with p99 60–100 ms.

## Mac routes

- From a fresh save: `DM_KEYS=f900:A*16/240` passes the title (Start Game) and starts a new game; the prologue's first scene is on screen by about frame 4,800. Scripted `LRight` and `A` did not move Ori there.

## Open

- **A crash at boot in about 1 run in 5** (4 of 23 runs): `sig=11` at `main+0x3e540` (also `+0x3e528`, `+0x3e5d0`; `lr main+0x3e5b4`), right after `SetThreadActivity` pauses and resumes thread 0x1003 (IL2CPP's garbage collector stopping the world). Not investigated.
- **A crash in Metal while the pipeline archive was written** (1 run): `dispatch_retain` under `-[_MTLBinaryArchive materializeAirNTFromOnlineForSlice:entryList:]`, during the prebuild of every shader pair into an empty archive. Not reproduced.
- The seed list covers only the start. A full playthrough on the Mac, recorded into `pso_user.tsv`, is planned, to extend it to every area so that a first start has no pipeline stutters anywhere.
- iPhone: not tried; build an IPA and record its size for the README gallery.
- Audio was not checked: the Mac host's default sink is silent.
