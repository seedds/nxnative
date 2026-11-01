# Taiko Risshiden V DX (`0100346017304000`)

Update 1.1.1 (v131072) over the base game, SDK 12.3.3 (no other game's `sdk` build). Koei Tecmo's KTGL engine on NVN, mostly 2D. Modules `main` (15 MB), `subsdk0` (the movie library, NintendoSDK_movie: the WebM opening and ending movies) and `sdk`. In Japanese, Simplified and Traditional Chinese only (no English), so the NSP script packs Japanese (`DM_LANG` in `Documents/env.txt` switches). Plays on the iPhone (IPA 3.3 GB) by touch in handheld mode; 60 fps.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Frame limiter threshold 0 when the game's vsyncs per frame N = 1 (`main+0x443fac`) | [`patches.py`](patches.py) `CUTS`, `main` build `f5ab74b9…`; other builds through `MATCH_CUTS` (1.1.1's function and the base game's) | its wait from the frame's start tick made every frame 18.2 ms (55 fps); the present pacing holds one vsync, and N = 2 scenes keep the wait; see [Frame rate](#frame-rate) |
| `op_mode handheld` | [`game.tsv`](game.tsv) | handheld, the Switch build plays by touch; docked it reads only the pads |
| `pad_layout touch` | `game.tsv` | the Tap layout sends every touch to the game (the pads' corners would cover the cards, the menu icons at the top right and the on-screen B button) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource (META +0x14 = 0): no virtual address memory; `nninitStartup` sets a heap of the pool − used memory (3.14 GB), which the game maps only as it touches it. The default 64 MB of used memory fits its small modules |

## Frame rate

- **Resolved: 55 fps with every frame 18.2 ms, the main thread asleep most of the time** (the menus and the town; the opening movie, paced by its own player, ran at 60). KTGL's frame end (`main+0x443e8c`) presents, acquires the next texture (the present wrapper `main+0x4fcf4c`), then waits until N / 60 s have passed since the frame's start tick, polling `nn::os::GetSystemTick` and sleeping 2 ms per round (`main+0x4c9f24`, `SleepThread`). The start tick is read at `main+0x443d44`, after the scene's update (the frame object's vtable +0x10, `main+0x443cec`).
- **Cause, in the game's code:** each frame therefore lasts N / 60 s plus the time from the wait's last poll to the next start tick, rounded up to the 2 ms rounds. That is never exactly one vsync, on any hardware. The host's `SleepThread` (`sleep_precise`) and tick are exact, and a model of the loop gives 54 fps both with the host's acquire, which returns at once when a frame is late, and with the Switch's double-buffered one, which blocks until the presented frame is on screen and so only moves where the rounds start. On the Mac, without a cut: 54.6 fps at the title, frame time p5 18.15 / p50 18.28 ms, every 11th frame on screen for two vsyncs.
- **N:** the int at GOT 0xa1c088 (data `0x9ec764`, initially 1) is the game's vsyncs per frame. `CPrivateBattleWindow` and `CPrivateBattleTutorialWindow` set it to 2 when they start (`main+0xf02d0`, `main+0xec4cc`), and `main+0xf04f4` puts it back to 1. The scenes' code counts 60 / N frames a second (`main+0x2f29c8`, `main+0x3106dc`). The game presents at interval 1 in every scene, so in those scenes only this wait holds a frame for two vsyncs. N stayed 1 in probe runs through the menus, the town, the field battle tutorial, a council, the castle training ground and the world map. The `CPrivateBattle` scenes, small unit battles by their assets (`PBBATTLE/PBUNIT_*`), have not been reached on the Mac yet (open). `main+0xea8b0` starts one and is called from about 50 event sites; the tutorial version comes from an event-script command (`main+0x163dd8`).
- How it was found: sampling the host (`sample <pid>`: 84% of the main thread in `dm_guest_sleep_ns` under `main+0x443d74`, 15% in `nvnWindowAcquireTexture` under the present wrapper), the per-frame times (`DM_NVN_TSV`), and probes on N's writers.
- **Current:** a cut at `main+0x443fac` computes the threshold N × tick frequency / 60 with `udiv` (the compiler's multiply by 1/60 took one more instruction), then sets it to 0 when N = 1. The wait then returns after its first tick, which it still stores, and the host's present pacing (the acquire waits) holds the frame to one vsync. With N = 2 the wait stays the game's own. The first cut (`b.hs` → `b` at `main+0x443fd4`) skipped the wait for every N: with N forced to 2 at the title it presented 60 frames a second against the game's 28.8, for scenes that count 30. At the title with the current cut: N = 1 gives 60.0 fps (frame time p50 16.67 ms, wakeups 2,050 → 300 a second); N forced to 2 gives 28.9 fps, the game's own pace. In the town: 60.0 fps, frame time p50 16.66 / p95 17.08 ms, about 300 wakeups a second, no red line.
- **Other builds (current):** `MATCH_CUTS` carries the cut to builds without registered cuts, with two implementations of the frame end under one rule ([function-matched cuts](../../../docs/reference/rewriter.md#function-matched-cuts-for-unregistered-builds)). One is 1.1.1's function (364 bytes, the cut at +288). The other is the base game's (v0, `main` `47d7baf6…`): `main+0x43e140`, 172 bytes, because 1.1.1 added the frame-skip branch at `main+0x443ed4`. It has the same 20 bytes at +104 (`main+0x43e1a8`), and in v0 too N is a global (GOT 0xa1be28, data `0x9ec55c`, initially 1) set to 2 only by the same two windows (`main+0xefe90`, `main+0xec0bc`), and the start tick is read after the update (`main+0x43e128`). `nso_fnmap.py` finds no exact match for 1.1.1's function in v0; v0's was found by disassembly. Through the rewriter, v0's `main` takes the cut at `main+0x43e1a8` (implementation 2), and a copy of 1.1.1's `main` with another build ID takes implementation 1 with output byte-identical to the registered cut. The base game has not been run (its `sdk` and `subsdk0` are 1.1.1's builds).

## Input

- **Current:** handheld (`op_mode handheld`, `pad_layout touch`), the game plays by touch: tap the cards and buttons, drag or flick the map. On the iPhone those are all the touches in the Tap layout, on the Mac the mouse. The layout button brings back the gamepad (the map scrolls with the right stick in the 3D layout; L / R and ZL / ZR switch tabs and lists). It draws 1920x1080 in both modes.
- **Resolved: taps in handheld mode did nothing, though the game reads the touch screen** (the title's PRESS ANY BUTTON, the menus' cards). Its touch handler (`main+0x42f1ac`) uses `GetTouchScreenState` only to track the finger and takes its taps, pans and swipes from `hid::GetGestureStates` (it acts on types 5–9), which the host answered with no entries. The host now recognizes gestures from the same touch points (`hle_touch.c` `gesture_sample`, unit test `t_gesture`). Every menu, the cards, the map's drag and flick worked by touch with it. `DM_TOUCH_LOG=1` logs each gesture entry, and a `DM_CTL` line `drag <x0> <y0> <x1> <y1> [seconds]` moves a finger for testing pans and swipes on the Mac.

## Services

- **Resolved: null read at `sdk+0x362b8` (`apm::SetCpuOverclockEnabled`) in the first calls of `nnMain`** (`main+0x2e1e4c`). `oe::SetCpuOverclockEnabled(bool)` goes through the apm session the host never opened. The host keeps the flag and the query reports it (`hle_oe.c`); it does not scale clocks.

## Files and shaders

- Loose files, 3.3 GB: KTGL G1T textures (some in MRLK packs), the game's own TR5 / TE5 / TS5 data, KTSS Opus streams (`.kno`, through `nn::codec`) and KTSL wave banks (`.wbd` / `.whd`). `romfs_marker` is `TDATA/BUSTUP.G1T`.
- Shaders: 212 GLSLC outputs: 45 in `main` and 167 in the G2S container `eff/KTGLEffect.g1s` (`_S2G3000`, as Age of Calamity's), read by the existing splitter; nothing is compiled at run time. The game is almost all 2D sprites (two draws a frame in the menus).

## Status on the Mac

- The notice, the logos and the opening movie, the title, a new game (1560, Kinoshita Tokichiro) through the opening story, the town, the council with a battle proposal, the territory map, a rest at home, a save and a load, and the field battle tutorial, docked with buttons and in handheld mode by touch.
- 60 fps (frame time p50 16.66 ms, p95 17.08 ms in the town), sound; about 0.7 GB resident and 1.6 GB of Metal allocations, flat over 15 minutes. No red line on that route. The prewarm list covers it (18 lines).
- **Not a host bug:** the town's top-right panel runs off the screen on a council day (its "council today" text is longer than the usual "n days left"): the panel starts where the manual's screenshot has it and widens for the longer text; on other days it fits.

## Mac routes

- The work folder's `run/ctl.sh` drives a run started with `DM_CTL`. From an empty save: A skips the notice and the movie, A at the title and on New Game, then A five times (the scenario, its story, Tokichiro, his abilities, the settings) and about 150 presses of A through the story to the town (on the council day: A, Right ×3, A enters the castle).
- `run/save_ref` holds a save in the town on 1560 May 1.
- Handheld (`DM_OPMODE=0`), the same by touch: five taps at the centre (`T0.5x0.5`) reach the load screen of Continue, then `T0.5x0.29` and `T0.5x0.47` load slot 1.

## Found on this game

General host changes first needed here; each applies to every game:

- **`oe::SetCpuOverclockEnabled`** (`hle_oe.c`) and **gestures for `hid::GetGestureStates`** recognized from the touch points (`gesture_sample`: yuzu's rules plus an 8 px slop, `t_gesture`, `DM_TOUCH_LOG`, `DM_CTL` `drag`): [CPU overclock calls without an apm session](../../../docs/reference/services.md#cpu-overclock-calls-without-an-apm-session), [Touch gestures recognized from the touch points](../../../docs/reference/services.md#touch-gestures-recognized-from-the-touch-points).
- **KTGL's frame-end wait** (a game-code cause; this profile's cut, no host change): [A frame-end wait polled in 2 ms sleeps](../../../docs/reference/frame-rate.md#a-frame-end-wait-polled-in-2-ms-sleeps).
- **Same-name implementations of a function-matched cut** (`nso_match.py`: exactly one must match, no red line for the others): [Function-matched cuts for unregistered builds](../../../docs/reference/rewriter.md#function-matched-cuts-for-unregistered-builds).
