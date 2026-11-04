# Mario & Luigi: Brothership (`01006D0017F7A000`)

The base game, 1.0.0 (v0), SDK 12.3.2 (the same `sdk` and movie builds as OCTOPATH TRAVELER II and TRIANGLE STRATEGY). Acquire's game on Unreal Engine 4.27.2 (`SDK MW+EpicGames+UnrealEngine-4_27_2`, project `Vermillion`) with an IoStore container, NVN, PhysX, Wwise, and Bink movies drawn through a material. Modules `main`, `subsdk0` (the movie module) and `sdk`. 60 fps (30 on the Switch).

Status: plays on the Mac. Headless runs on 2026-11-04 went from an empty save through the autosave notice, the opening movie, the title, a new game and the beach scene to free walking, then the first battle (the tutorials, jumps with timed stomps, enemy turns and jump counters, both enemies defeated, the results screen), and a load of the autosave. No red lines in those runs at 30 or at 60 fps. Sound was measured, not heard. Anything past the first battle's results is untested. No IPA has been built and nothing has run on an iPhone.

## What it needs

No rewriter cuts of its own and no `patches.py`.

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game sets near = 0 at startup (log: `depth mode near=0`); the NSP script's heap check found it |
| `fps_target 60` | `game.tsv`, `hle_fpslock.c` | the game's device profile sets `rhi.syncinterval=2`; see [Frame rate](#frame-rate) |
| `fps_max 120` | `game.tsv` | **Frame rate** offers 120 on the Mac: speed measured; see [Frame rate](#frame-rate) |
| FMallocBinned3 constructor call cut, engine region given what the alias region has room for | the rewriter's engine-wide rules (`dm/tools/ue_malloc.py`, `dm/tools/ue_region.py`), no build ID | as the other Unreal 4.27 games (log: `62.00 GB asked, 2.81 GB of address space`) |
| Node area cut 1 GB → 62 MB, regions aligned to 2 MB | `dm/tools/sdk_patches.py`, the SDK-wide rules on `sdk` build `5bcf525f…` | the same `sdk` as OCTOPATH TRAVELER II |
| 3.75 GB guest block: `mem_reserve 0xF0000000`, `mem_alias 0xC8000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the layout of the other Unreal 4.27 games, in place of the provisional 64 GB alias region |
| `sys_resource 0x5000000` | `game.tsv` | the NPDM declares 80 MB of system resource |
| `pad_layout full` | `game.tsv` | the input config (`Vermillion/Config/DefaultInput.ini`) binds the D-pad and the right stick |
| `shader_groups GlobalShaderCache=global #Engine/=engine #Vermillion/=game` | `game.tsv` | the global shader cache, the engine's materials and the game's |

## Versions

- **Current: the base game only.** The update in hand, 1.0.1 (v65536, `main` build `bf0607ac795b593a3f685934c249147c`, the same `sdk` and `subsdk0`), is damaged: its Program NCA `074db5a4…` does not hash to its content ID (the other NCAs of both NSPs do), and its RomFS's own tables fail the hash check. Pack with `--package v0`. A good copy of the update packs from a profile derived from this one, as any unregistered version does; that has not been run.
- The survey reads the newest version whatever `--package` says, so the first survey needed a folder with the base NSP alone. Once this profile exists there is no survey.

## Frame rate

- **Current:** 60 fps (30 on the Switch). The game's device profile inherits `rhi.syncinterval=2` (`Engine/Platforms/Switch/Config/BaseSwitchDeviceProfiles.ini`), and its configs set no fixed frame rate. The host writes 1 whenever the game sets it (`main: Unreal rhi.SyncInterval … 2 → 1 vsyncs`).
- The same scripted route from an empty save, run side by side at 30 and at 60 fps (`run/ab.sh`), shows the same stage at the same times in all seven shots compared, from the movie's end through the title to the beach scene, at 30.0 and 59.8 fps: the game steps by the time that really passed. The first battle was played through at 60 fps (a timed stomp gave its EXCELLENT, the jump counters landed), and at 30 fps up to its stomps and enemy turns, before that run's time limit. `DM_FPS_TARGET=30` keeps the game's own 30.
- At 60 fps the windows of 300 frames stay at 58–60 fps; the dips to 49–54 are scene starts and the runs' own screenshots.
- **Current: 120 fps on the Mac when chosen** ([120 fps follows the frame step](../../../docs/reference/frame-rate.md#120-fps-follows-the-frame-step)), the first Unreal game. The game also sets `t.MaxFPS` to 30 at run time, and the host's write follows the hold (`main: Unreal t.MaxFPS … 60 → 120 fps`). This holds in no particular version, since the rewriter finds Unreal's variables by name in any build; 1.0.0 is the one run, as the update in hand is damaged. The speed checks and first frame rates below ran headless on an M6 on 2026-11-05, one run at a time, from `run/save_ref`, with no red lines, and with another session's game running on the same Mac (scripts in the work folder's `run120/scripts`).
  - Speed, walking: the same timed walks (0.3 to 1.5 s, out and back), twice at each rate, covered 1,376 px of scene at 120 and 1,376 px at 60. Hold by hold the ratio is 0.95–1.04, within what repeats at one rate differ by (one hold 237 and 277 px apart at the same rate), except a 0.5 s hold that scrolled the scene about 0 px at both rates.
  - Speed, the first battle: the same walk reaches the same places and the same battle at both rates. With no press during them, the stomp (14.5–19 s, shots every 0.2 s) lines up with no shift between the rates and the enemies' turn ends at 29.4 s in all four runs. One 120 run's stomp came 2.0 s early, the spacing of the presses, so a press landed one earlier; the two runs at 120 differ by the same. This rules out double or half speed, not a difference of a few percent.
  - Frame rate: 119.4 fps in most windows of 300 frames on the beach and in the battle, and 60–72 for about 30 s of one battle (the host holds 60 when frames are late). Not measured with the Mac otherwise idle. CPU energy about 2.0–3.0 W against 1.1–1.5 W at 60.
  - In a window on a 120 Hz screen, with the Metal HUD and sound (about 3 minutes, same day, no other game running, no red lines): 119.3 fps at the end of the run, 2.16 W of CPU energy, and of 67 windows of 300 frames the slowest 1% took 8.33 ms in 36 and 16.67 ms in 23. The other 8 reached 25 to 108 ms; what was on screen then is unknown. The frame step fell to hold 2 five times and came back each time, the longest for about 560 frames, near the start.
  - Not measured at 120: timed stomps and counters landing (the presses were blind), the opening's Bink movie.

## Memory

On the Mac, on the small layout, through the first battle: 4.2 GB resident with 1.6 GB of Metal allocations at 60 fps (3.1 GB resident at 30 fps at the same stage of an earlier run), the alias region mapped up to +1.25 GB of 3.12 GB, the Unreal heap up to 1.00 GB. Not measured over a long run or on an iPhone.

## Files and shaders

- `Vermillion/Content/Paks/Vermillion-Switch.ucas` / `.utoc` (6.9 GB, TOC version 3, Oodle in 256 KB blocks, not encrypted, 56,109 chunks: 52,123 export bundles and 3,985 bulk data) hold the cooked packages. `Vermillion-Switch.pak` (version 11, 477 MB, 4,461 files, index not encrypted) holds the rest: Wwise banks and `.wem` files, fonts, localization, configs and the global shader cache. 111 `.bk2` movies lie loose in the RomFS.
- Shaders: 45,824 unique (13,581 vertex, 28,296 pixel, 3,814 compute, 133 geometry), inline in the export bundles: [Unreal Engine 4.27 shaders inline in IoStore export bundles](../../../docs/reference/shaders.md#unreal-engine-427-shaders-inline-in-iostore-export-bundles). The split takes 39 s. A first full Mac build compiles them in about 45 minutes on an M6 (10 GB of libraries).
- The movies draw through a material, `Vermillion/Content/UI/Movie/MovieBinkMaterial.uasset`, so no shader of their own had to be found.
- Prewarm list ([`pso_manifest.tsv`](pso_manifest.tsv), 3,883 lines): recorded on the Mac with prewarm off over the boot, the opening movie, the title, a new game, the beach, a load of the autosave, the walk to the first battle, the battle and its results.

## Sound

Three outputs: renderer 0 (48 kHz, 32 voices, `main+0x358ca1c`), whose frames are not silent only during the movie; Wwise's own AudioOut (`main+0x365061c`, beside `AK::EventManager` in `main`); and Unreal's mixer on renderer 1 (32 kHz, 64 voices, `main+0x30a4ff0`), silent through all 108,000 frames of the movie, the field and the battle. With the renderers silent in the field, the sound there is Wwise's: no clipping, RMS about 1,000 in `DM_AUDIO_DUMP`, which interleaves all three outputs. The sound itself is still to be listened to.

## Input

- A notice about autosave comes first on an empty save (A). The opening movie, about 2.5 minutes, took neither A nor Plus as a skip. Blind A presses at the title start a new game, and pass the beach scene.
- With a save the title goes to Load (A, A); control comes back in about 40 s from boot, with no movie.
- On the beach the crabs are scenery, and the rock wall north of the start is passed at its west end, over the round ledges. The left stick walks relative to the camera. The first battle starts on touching a sprout on the upper ledge.
- In a battle, A opens Jump, chooses the target, and lands the stomp and the jump counter when pressed in time. Tutorial pages turn and close with A.

## Mac routes

The work folder's `run/` has `host.sh` (copies a host to `build/mlhost`), `boot.sh <log> [seconds] VAR=…` (headless), `see.sh`, `shots.sh`, `mash.sh`, `sum.sh`, `stop.sh`, `to_battle.sh` (from control at the beach start to the first battle), `fight.sh` (A once a second, with shots), `ab.sh` (the frame-rate comparison) and `rec.sh` (recording the prewarm list). `run/save_ref` holds the autosave at the beach start.

## Found on this game

General changes first needed here; each applies to every game:

- **Inline shaders with the cooked packages in an IoStore container** (`uepak.py`, `shader_split.py`): [Unreal Engine 4.27 shaders inline in IoStore export bundles](../../../docs/reference/shaders.md#unreal-engine-427-shaders-inline-in-iostore-export-bundles).
- **More than one audio renderer** (`hle_audren.c`): [A game with more than one renderer](../../../docs/reference/audio.md#a-game-with-more-than-one-renderer).
- **An update whose RomFS tables fail the hash check** (`nx_extract`): [A damaged RomFS's own tables](../../../docs/reference/packing.md#a-damaged-romfss-own-tables).

## Open

- Past the first battle: nothing is tested. No IPA, no iPhone run.
- The sound has not been listened to, and no route was found to make Unreal's renderer sound.
- The update: see [Versions](#versions).
