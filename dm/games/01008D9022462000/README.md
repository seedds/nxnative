# The Rogue Prince of Persia (`01008D9022462000`)

Update 1.1.0 (v262144) over the base game, SDK 20.5.29. Evil Empire's game on Unity 6000.0.64f1 IL2CPP with FMOD (one AudioOut) on NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`, plus one NRO plugin in the RomFS. It plays on the Mac at 60 fps (30 on the Switch), up to 120, with no game-specific rewriter cut and no host change of its own: the frame rate goes through Unity's settings, found by the engine's internal-call table. Reached: the title, the first-run options, the save slots, the opening narration, the intro movie and the first level, with running, jumping, climbing, the weapon, the wall run and the pause menu. No enemy was met. Not run on an iPhone or iPad.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, and the hardware `app_pool 0xCD500000` | `game.tsv` | the NSP script's guess, which its address region check accepted and which held in play; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0, so the pipeline bases made before it runs take that mode |
| `shader_groups /Data/=data /.nrr/=nrr /Plugins/=plugins` | `game.tsv` | the guess; every shader is under `Data/` or in `main`, so the last two groups find none. They stay because `shader_*` keys are in every shader's cache key, and dropping them would build all 41,640 shaders again for the same output |

## Frame rate

- **60 fps (30 on the Switch), up to 120 on the Mac** (`fps_target 60`, `fps_max 120`; `DM_FPS_TARGET=30` gives the Switch's rate). The game sets Unity's `QualitySettings.vSyncCount` to 2 (present interval 2) and leaves `Application.targetFrameRate` at -1. The rewriter finds both through Unity's internal-call table, with no build ID, and the host writes the count and presents at the target ([Unity's vSyncCount and targetFrameRate](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table)). GPU time is about 2 ms a frame at the first level.
- **Speed:** Unity steps the game by the time that really passed. A timed walk from the same save (1 s, then 2 s of the stick) ends at the same place at 30 and 60, and the user found the speed right at 120 (2026-11-08).
- **Measured** (headless, then windowed with a 120 Hz screen): 30 fps p50 33.33 ms; 60 fps p50 16.6 ms; 120 fps 8.33 ms in every window through the title, the opening and the first level, 60 while the intro movie plays (the frame step caps movies). 120 needed the display's vsync event at 120: Unity's NVN device waits for each vblank ([The display's vsync event follows 120 fps](../../../docs/reference/frame-rate.md#the-displays-vsync-event-follows-120-fps)).

## Memory

- **Current:** at the first level, resident 2.47 GB and 0.90 GB of GPU memory, flat over the play. The alias region held up to 1.77 GB mapped of 4.38 GB.
- Later biomes are not measured.

## Files and shaders

- The RomFS has 470 files, 0.77 GB: 369 UnityFS bundles and 74 FMOD banks, with one NRO plugin under `Plugins/` that the NSP script rewrites with the modules. No DLC.
- 41,640 shaders (18,702 vertex, 18,706 pixel, 4,231 compute, one geometry): 41,629 from `Data/` and 11 GLSLC outputs in `main`. All built for the Mac, none failed, and the build reports nothing lost without failing.
- The prebuild makes pipeline bases from 18,126 shader pairs on the Mac: 11,118 built, 7,008 left to the draws, none failed.
- `pso_manifest.tsv` is empty: no run recorded one.

## Sound and movies

- The intro movie (the SDK's movie library) plays with picture and sound; its frames were checked by screenshot.
- A `DM_AUDIO_DUMP` of a 236 s run has sound in 180 seconds, from the opening narration through the movie and into the first level. The boot, the title and the menus before it were silent, which was not compared with a Switch. Every run used the silent sink, so playback through the Mac's speakers was not heard.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command.
- From a fresh save: about 25 s to the title, Plus, A on the health warning, then the first-run options: `LDown`, `LDown`, A on Play (X on that screen asks to restore the defaults instead). A loading screen, then about 16 A presses through the narration, the publisher's logo and an intro movie of about 90 s, then A through the prince's first lines.
- With a save: Plus, A on the warning, A on the slot. The save made during the opening has the same opening again.
- Controls (the pause menu's Options, Controls): B jump, Y weapon, A tool, ZR dash / vault, ZL wall run, X kick, L heal, R interact, Minus the bag, Plus pause. A ledge needs B held with the direction (`B+LRight~2.5`); ZL with a jump against a wall clings to it.

## Open

- No enemy was met, so combat, taking damage and death are not checked; nor are later biomes or a run's end, at any frame rate.
- The title and the menus before the narration are silent in the audio dump.
- iPhone and iPad runtime validation; no IPA has been built.
- Input has gone only through `DM_CTL`, not a controller or the keyboard.
- The base game alone was not played or registered; packing it takes a derived profile.

## Found on this game

- [Unity's vSyncCount and targetFrameRate, found through the internal-call table](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table).
- [The display's vsync event follows 120 fps](../../../docs/reference/frame-rate.md#the-displays-vsync-event-follows-120-fps).
