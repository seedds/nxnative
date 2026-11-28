# stitch. (`010025801CEA8000`)

Hitcents' yarn puzzle game, update 1.2 (v65536) over the base game 1.111, with its two DLC, `010025801CEA9001` and `010025801CEA9002`. SDK 21.4.4, NVN. Unity 6000.0.68f1 IL2CPP with FMOD. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays by touch in handheld mode, which the profile makes the default: the tutorial was played through on the Mac by mouse, to the level menu, and the level menu's first puzzle was started from that save and stitched into. 60 fps by default (as on the Switch); the player's **Frame rate** takes 30. A double-clickable Mac app is packed.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `op_mode handheld` | [`game.tsv`](game.tsv) | the game reads the touch screen only in handheld mode: see [Touch](#touch). Handheld renders 1280x720: the game makes 1920x1080 window textures and crops them (`WindowCrop 0,0 1280x720`). Docked gives 1080p, with pads only |
| `pad_layout touch` | `game.tsv` | the iPhone's virtual gamepad starts in its Tap layout, every touch to the game |
| `present_interval display`, `fps_target 60` | `game.tsv`, `hle_nvn.c` | 60 fps by default; the player's **Frame rate** picks 30 or 60 ([Frame rate](#frame-rate)) |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NSP script's layout for a game with a system resource; the pool is the Switch's |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (the NSP script's heap check) |
| `shader_groups /Data/=data` | `game.tsv` | the Unity shader objects are in the player's serialized files and bundles under `Data/` |

No code of the game is cut. The 30 fps rewriter cut that was tried before this profile (2026-11-08, `main+0x7db590`) is not part of it: the cap is the present interval alone ([Frame rate](#frame-rate)).

## Touch

- Docked, the Switch's touch screen is off and the game follows: it reads only the pads then. In handheld mode the stitches are drags. On a fresh save the game opens on the tutorial's step 1/8: `drag 0.475 0.5 0.53 0.5 1.0` (`DM_CTL`) makes the first stitch, then the camera moves for about 3 s to step 2/8.
- Controllers work too (the screen shows "L Move Cursor" and "A Start Stitch"); the in-game layout button brings back 2D / 3D for a controller player.
- On the Mac the mouse is the touch screen ([Input](../../../docs/development.md)), in handheld mode only, which `op_mode handheld` makes the default.

## Memory

- The NPDM declares 16 MB of system resource and `main` never sets a heap: the game takes its memory as address regions. The NSP script's check boot saw alias +2.23 GB in its first 15 s in nnMain, of 4.38 GB.
- **Current (2026-11-26, main `e9bcc5f3` with this profile):** with the iPhone's alias region (`DM_ALIAS_SIZE=0x118000000` on the Mac) the high-water mark is +2.26 GB of 4.38 GB over the tutorial route (about 80 s, 4,500 frames), with 1,684 MB resident at the end. With the Mac's own 64 GB region, a copy of the play save, into the level menu's first puzzle, reached +2.50 GB, with up to 1,902 MB resident (about 100 s). A long session was not measured.

## Files and shaders

- 3,153 shaders: 1,540 vertex, 1,544 pixel, 68 compute and one geometry shader, read by the shader split from the player's serialized files and bundles. All built for the Mac with no failure, and no run met a shader missing from the map. The one geometry shader's mesh library is built by the split from its own side table (`geo_pairs.tsv`, not committed).
- The prewarm list (`pso_manifest.tsv`, 120 keys: 81 shaders and 39 render pipelines) was recorded on the Mac as the device list `pso_user.tsv` (beside a fresh scratch save, with the bundled list empty, so every line is an item built on demand): 108 lines from the tutorial route's first stitch, 12 more from the tutorial played through by mouse to the level menu (upstream `f56fddeb`, the same game build and shader map). A first start prewarms them (`pipeline prewarm done: shaders 81 / render pipelines 39 ... shader keys not in the shader map 0`). It does not cover the puzzles: a run into the level menu's first puzzle appended 7 more lines.

## Frame rate

- **Current: 60 fps** (as on the Switch), 30 with the player's **Frame rate** (`DM_FPS_TARGET=30`). The game asks for present interval 1 and never changes it (no `the game sets present interval` line in any run). It steps by the time that passed. This main has no engine frame limit the host writes: `dm/tools/unity_fps.py` finds no internal-call table in this `main`, and Unity's NVN device takes the quality level's vSyncCount once, at creation ([#1812](https://github.com/seedds/nxnative/issues/1812)). Unity 6000.0.50 to 64 builds upstream have the table (Hollow Knight, Hollow Knight: Silksong, The Rogue Prince of Persia); 6000.0.68f1's `main` has none that the tool reads. So a cap applies only in display mode, by the present interval alone ([A present interval that is the game's only limit, in display mode](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode)); at 60 display mode changes nothing, as the game asks for 1 itself.
- **Measured (2026-11-26, Apple M5 Max Mac Studio, macOS 27.2, window visible, handheld, a fresh scratch save, the tutorial route: step 1/8 until the drag about 50 s after the start, with two shots, then shots and about 25 s more; the host's 300-frame windows):**

  | | 30 (`DM_FPS_TARGET=30`) | 60 (default) |
  |---|---|---|
  | main `e9bcc5f3` + this profile, frame time p50 / p95 | 33.32 to 33.34 / 33.5 to 33.6 ms | 16.66 to 16.67 / 16.8 to 17.1 ms |
  | main `e9bcc5f3` + this profile, CPU energy, idle tutorial step | 0.12 to 0.17 W | 0.16 to 0.23 W |
  | main `e9bcc5f3` + this profile, share on P-cores | 19 to 28 % | 16 to 76 % |
  | main `f56fddeb`, display + 30 / display + 60 / default 60, frame time p50 / p95 | 33.33 / 33.5 to 33.6 ms | 16.66 / 16.78 to 17.9 ms |
  | main `f56fddeb`, CPU energy, idle tutorial step | 0.10 to 0.16 W | 0.22 to 0.26 W |
  | main `f56fddeb`, share on P-cores | 24 to 34 % | 63 to 95 % |

  The p95 leaves out the window of the drag and the camera move (42.6 ms at 30 and 33.6 ms at 60 on `e9bcc5f3`, 39.6 and 33.5 to 33.6 ms on `f56fddeb`). The cap holds on both builds. At 30 the idle step's CPU energy is lower than at 60 by about 45 % on `f56fddeb` and about 30 % on `e9bcc5f3`, whose runs at 60 spent less of their time on the performance cores; the step is short and that share swings a lot from run to run. The log lines are `present tick 60 Hz, interval mode display (one frame per tick, a cut) (game profile present_interval; present interval set by the game 1; window)` with `present cap 60 fps (game profile fps_target) applied by the present interval alone`, and with 30 `present cap 30 fps (DM_FPS_TARGET) applied by the present interval alone`. Unity's own wait at 30: the main thread blocks in the acquire (NVN submit + present p50 0.3 to 0.9 ms a frame), and Job.Worker 0's back-off sleeps stay at about 3,100 to 3,300/s at both rates (no spin).
- **Speed:** shots 0.25 to 5 s after the same tutorial drag show the same camera positions at 30 and at 60 (and with `DM_ALIAS_SIZE`), and the tutorial moves from 1/8 to 2/8 in all of them: the game does not count frames.
- **Idle in a puzzle:** about 20 s after the last input in a puzzle, the game presents only every 20th display tick, 3 fps (`[longframe]` lines of 333 ms and `acquire thread … has not acquired for 3xx ms`), with the present interval still 1 and no host line about the cap; the next tap brings 60 fps back at once. Seen in both runs into the level menu's first puzzle at the default 60 (main `e9bcc5f3`); the tutorial's step 1/8 did not do it in about 50 s of idling. It is the game's own idle mode as far as its IL2CPP metadata shows: it names `FPSController` with `CanTriggerLowFPS`, `isLowFpsOn` and `ToggleLowFps`, and Unity's `OnDemandRendering.renderFrameInterval`, which renders only every Nth frame. Not compared with a Switch.
- **Handheld is 720p** (see [What it needs](#what-it-needs)), so the picture is 1280x720 on the Mac and the iPhone. Docked gives 1080p but no touch.
- `DM_FPS_HOLD` ("Hold frames when late") is listed for every `fps_target` game; here, with no frame step, it does nothing.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app`; the profile needs no option.
- From a fresh save, window visible, handheld by default: wait about 30 s for the tutorial's step 1/8, then `DM_CTL` `drag 0.475 0.5 0.53 0.5 1.0` makes the first stitch and the camera moves to step 2/8 (shots 0.25 to 5 s later).
- From a save past the tutorial: the game opens on the level menu; a tap on its first puzzle (`T0.305x0.455`) and on the play button (`T0.5x0.556`) loads it after about 10 s, and `drag 0.24 0.74 0.45 0.74 1.5` stitches the 6 at the bottom left over two cells.

## Testing

- 2026-11-26, Mac (Apple M5 Max Mac Studio, macOS 27.2), main `e9bcc5f3` with this profile (uncommitted at the time), the app packed from the repository's profile (log: `game profile: 010025801CEA8000`, not provisional), a fresh scratch save, window visible, handheld by the profile default (no `DM_OPMODE`, no `DM_PRESENT_INTERVAL`): the tutorial route at the default 60 and at `DM_FPS_TARGET=30`, one more at 60 with `DM_ALIAS_SIZE=0x118000000`, each about 80 s with 0 `[RED]` lines and a graceful exit. Then twice from a copy of the play save (the tutorial cleared): the level menu's first puzzle started and a drag made a stitch, about 100 s in all at the default 60 (40 to 55 s of it in the puzzle; the second run idled until the game went to 3 fps, then a tap), 0 `[RED]`, graceful exit. The prewarm list loaded with its 120 lines.
- 2026-11-26, Mac, main `f56fddeb` with the NSP script's provisional profile and the same two frame-rate keys given as environment variables: the same route at 30, display + 60 and the default 60, and a hand-played session by mouse in handheld at 30 (zh-Hant): the tutorial, played through to the level menu (the game saved its end), about 3,900 frames, frame time p50 33.33 ms, p95 up to 33.6 ms (one 245 ms frame at the load after the tutorial), 0 `[RED]`, graceful exit.
- Not tested: an iPhone with this profile (the last iPhone build was `c78d7375` with the 30 fps cut of 2026-11-08), the puzzles past the first one's start, DLC content, controllers beyond the tutorial, a long session's memory, and the Docked mode's 1080p picture.
