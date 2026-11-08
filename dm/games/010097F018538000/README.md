# DAVE THE DIVER (`010097F018538000`)

Update 1.0.6.1267 (v3080192) over the base game, SDK 20.5.29. Unity 6000.0.52f1 IL2CPP with FMOD (one AudioOut) on NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. Plays on the Mac: the DLC notices, the title, a new game, the opening, the first dive with its tutorial (swimming, the knife, the harpoon, catching fish, the oxygen warning), the boat, and the sushi bar up to Cobra's first lines there; `Continue` after a restart loads the save. The play test stopped there at the user's request, so serving customers was not reached. No game-specific rewriter cut and no host change of its own. iPhone and iPad validation remains open.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, and the hardware `app_pool 0xCD500000` | `game.tsv` | the NSP script's guess, which its address region check accepted and which held in play; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (log: `depth mode near=0`), so the pipeline bases made before it runs take the `main0_z` vertex entries |
| `shader_groups /Data/=data` | `game.tsv` | everything with shaders is under `Data/`: the player's serialized files and the `StreamingAssets` bundles |
| No `pad_layout` (the 2D default) | `game.tsv` | see [Mac routes](#mac-routes) for the controls |

## Frame rate

- 30 fps, as the game asks: `present interval set by the game 2`, and frames stay at 33.3 ms from the title through the sushi bar. A windowless run's p99 frame interval is 33.5 to 38 ms. Running at 60 fps (`DM_PRESENT_INTERVAL=display`) was not tried.

## Memory

- **Current:** alias region high-water mark +2.51 GB of 4.38 GB with 1.51 GB mapped at the sushi bar. Peak resident 2.55 GB and 0.80 GB of GPU memory over the run from the title to the sushi bar. The hardware pool of 3.21 GB leaves room here, unlike Unity games whose allocator takes the whole pool at once ([Castlevania: Belmont's Curse](../0100BA4026E94000/README.md#memory)).
- Later areas and days are not measured, nor is the In the Jungle DLC.

## Files and shaders

- The merged RomFS has 4,004 files, 4.03 GB: 3,967 UnityFS bundles and two FMOD banks. Six DLC NSPs: indices 1, 5 and 6 are licences, and indices 2, 3 and 4 hold content. Three carry `[ERR]` in their file names, but extraction reports no damaged file. The host reports all six as owned, and the game announces them.
- 22,072 shaders (10,843 vertex, 10,847 pixel, 329 compute, 26 hull, 26 domain, one geometry): 22,061 from `Data/` and the player's 11 GLSLC outputs in `main`. All built for the Mac with no failure. 1,669 of them are in `Data/resources.assets`, which needed [a fix to the split](../../../docs/reference/shaders.md#unity-compute-kernels-with-no-code).
- The prebuild makes pipeline bases from 10,849 shader pairs on the Mac: none fail, 116 are left to the draws.
- `pso_manifest.tsv` is empty: no run recorded one.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command.
- **Many confirmations are held, not tapped.** `Normal Start` on the Select Starting Point dialog ignores a tap and a 0.5 s hold, and `A~3` starts the game, so a tap looks like a hang. A tap of X did nothing on `Skip to Jungle` either; a hold there was not tried. Hold X (`X~2`) to dive and to open the boat's destination menu, and hold A (`A~3`) to skip the opening credits. A tap of B leaves the dialog.
- From the start of the title: five A presses for the five DLC notices, which come back on every start, then A on `New Game`, `A~3` on `Normal Start`, `A~3` once the `Skip` prompt shows. About 24 A presses go through the boat dialogue.
- Dive: `LRight~1` to the dive point, `X~2`. In the water the left stick swims, B is the knife, and a held A aims the harpoon with the **left** stick while ZR fires (`A+L1/-0.4~0.8`, then `A+L1/-0.4+ZR~0.25`); the right stick does not aim. The tutorial refills the oxygen when it runs out.
- To the sushi bar, after the dive's dialogue: `LLeft` for about 2.7 s to the arrow, `X~2`, `X`, `X~1.5`.
- `Continue` resumes from the last autosave: a save made on the first boat restarts the boat dialogue.

## Found on this game

- [Unity compute kernels with no code](../../../docs/reference/shaders.md#unity-compute-kernels-with-no-code): 11 of 142 kernels in `Data/resources.assets` have none, which cost the split the whole file.
- [Handles built under a guard the read's guard cannot be tied to](../../../docs/reference/shaders.md#handles-built-under-a-guard-the-reads-guard-cannot-be-tied-to): 364 of its 22,072 shaders change, 319 of them read a texture word with no sampler word (mostly `Jungle_Dave_Uber`). None drew in the sampled frames of a run from `save_d1boat`, so no picture was compared.

## Open

- **Current (2026-11-05):** the translator deletes 32 bindless accesses in 2 shaders (2 pixel; texture2D 32), whose results read 0: `74cb82b3eb27955d` `dab363d965d1374a`. The cause is not traced, and whether a route reaches them is not known. `74cb82b3eb27955d` lost 17 before `84dea698`, the float comparisons that honour .FTZ, and 30 since; why is not traced. [Losses a shader build reports](../../../docs/reference/shaders.md#losses-a-shader-build-reports).
- The five DLC notices return on every start, even with a save. Whether the Switch does the same, or something the game stores does not persist here, was not looked into.
- iPhone and iPad runtime validation; no IPA has been built.
- Sound is not checked: every run was windowless with the silent sink. Input has gone only through `DM_CTL`, not a controller or the keyboard.
- The movie module decodes 1920x1080 at the start of the game, per the log, but no movie frame was looked at.
- Serving customers at the sushi bar, everything after the first evening, and the DLC stories.
- The base game alone was not played or registered; packing it takes a derived profile.
