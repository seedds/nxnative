# TUNIC (`0100DA801624E000`)

Update 1.0.3 (v196608) over the base game, SDK 14.3.0. Andrew Shouldice's game (published by Finji) on Unity 2020.3.36f1 IL2CPP with FMOD (one AudioOut) on NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. It plays on the Mac at 60 fps (30 on the Switch) with no game-specific rewriter cut and no host change of its own: the NSP script's provisional profile with `fps_target 60` added. Reached: the title, New Game, the opening fly-over, control on the beach, the stairs up the cliff and the plateau above it with its first enemies, and the pause menu. No fight was started. Not run on an iPhone or iPad.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | `game.tsv` | the NSP script's guess after its heap check (no heap set; with the hardware pool the alias region filled to +4.26 GB of 4.38 GB, so a 1 GB pool), which held in play; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0, so the pipeline bases made before it runs take that mode |
| `fps_target 60` | `game.tsv` | 60 fps instead of the Switch's 30; see [Frame rate](#frame-rate) |

## Frame rate

- **60 fps (30 on the Switch)** (`fps_target 60`; `DM_FPS_TARGET=30` gives the Switch's rate). The game sets Unity's `QualitySettings.vSyncCount` to 2 (present interval 2). The rewriter finds it and `Application.targetFrameRate` through Unity's internal-call table, with no build ID (1.0.3: `targetFrameRate` at `main+0x353c738`, `vSyncCount` at `[[main+0x38864a0]+0x30] + [[main+0x38864a0]+0x4c] * 0xa8 + 0x68`), and the host writes the count ([Unity's vSyncCount and targetFrameRate](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table)).
- **Speed:** Unity steps the game by the time that really passed. A timed walk from the start of a new game (3 s of the stick to the left) ends at the same place at 30 and 60 (2026-11-10).
- **Measured** (windowed, Apple M6, 60 Hz screen): 30 fps p50 33.35 / p95 33.82 ms; 60 fps p50 16.67 / p95 16.77 ms on the beach. GPU time about 6 ms a frame. 120 was not measured (the screen is below 120 Hz), so `fps_max` stays 60.
- In a run with `DM_CTL` `shot`s, a long frame of 60–190 ms followed each shot; runs without shots had no long frame after the New Game load (100 s on the beach, with and without sound): [Long frames a test run makes itself](../../../docs/reference/frame-rate.md#long-frames-a-test-run-makes-itself). The collector's stops are short: 2–18 threads paused per collection, each pause under 1 µs.

## Memory

- **Current:** from the title through the beach and the plateau, resident up to 2.26 GB and 1.27 GB of GPU memory, flat over a 4-minute run. The alias region held up to 3.02 GB mapped of 4.38 GB (the iPhone's size).
- Later areas are not measured; 1.36 GB of the alias region is left.

## Files and shaders

- The RomFS has 10,553 files, 2.75 GB: 87 Unity serialized files (`.assets` and their `.resS` / `.ress`), 135 FMOD banks (0.71 GB), and under `Data/GI` 10,042 small files (`.neo`, `.caw`, `.ecm`, `.rgb`, `.vis`, `.pos`; 0.59 GB). No bundles, no DLC.
- 35,753 shaders (17,845 vertex, 17,849 pixel, 56 compute, 3 geometry): 35,742 from `Data/` and 11 GLSLC outputs in `main`. All built for the Mac in 8 minutes, none failed, every binding decoded.
- The prebuild makes pipeline bases from 17,847 shader pairs on the Mac in about 2 minutes: 17,833 built, 14 left to the draws, none failed. A run whose working folder has no archive builds them while the game waits at boot; point it at the NSP script's `run/pso_archive`.
- `pso_manifest.tsv` is empty: no run recorded one.
- The opening sea is an opaque saturated blue with a bright shore line, as on the console: its draws are `Custom/WaterEdge` over the clear colour. `Shader Forge/TUNIC water` is not drawn there.

## Sound

- With `DM_AUDIO=1` the game opens one AudioOut (2 channels, 240-frame device buffer) and runs with no underruns; playback was not listened to or dumped.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command.
- From a fresh save: the title within 40 s (first frame at about 8 s), A on New Game, a fly-over of about 20 s, then control on the beach.
- From the start spot: the stairs are up and to the left; `LDown+LLeft`, `LLeft` about 1 s each, then `LUp+LRight` climbs them. The plateau above has the first enemies.
- Controls checked: the left stick, A on the title, Plus for the pause menu (Return To Game, Options, Quit To Title Screen).

## Open

- No fight, no item pickup, no save point and no area past the first plateau are checked, at any frame rate.
- Sound was not heard or dumped.
- iPhone and iPad runtime validation; no IPA has been built.
- Input has gone only through `DM_CTL`, not a controller or the keyboard.
- The base game alone was not played or registered; packing it takes a derived profile.

## Found on this game

Nothing new: the general mechanisms it uses are listed above.
