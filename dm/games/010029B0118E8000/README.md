# Need for Speed: Hot Pursuit Remastered (`010029B0118E8000`)

Update 1.0.3 (v196608) over the base game (v0), SDK 10.4.1. Criterion's engine (ported by Stellar Entertainment) on NVN, with Bink movies and Criterion's Bundle 2 (`.BNDL`) files. Modules `main`, `subsdk0` (GLSLC, NVN's runtime shader compiler; not loaded, nothing calls it), `subsdk1` (NintendoSDK_movie) and `sdk`.

Status: plays on the Mac (Apple M4, 2026-10-28): boot, the Autolog connection screen (offline, it goes on by itself), the career map, car select and whole races (Grand Ocean Road, Mystic Lake Road, South Face Drive, First Offence with the gold medal), at 60 fps with sound, a Switch Pro Controller and saves. In two sessions of 7 and 11 minutes: no crash, no new red line; at 60 fps 81% of the 300-frame windows held 16.7 ms (the others, menus and loading screens, 18 ms), 0.37% of the frames dropped, GPU 6.3 ms a frame (p50), the main thread about 4 ms.

A contributor's tests on 1.0.3 (routes and durations not recorded):

- **iPhone 18 Pro Max** (`iPhone19,3`, iOS 27.0.1, 2026-10-30; Xcode 27.1, a free Personal Team, `com.apple.developer.kernel.increased-memory-limit`): works and looks good. The log passed 6,000 frames with no crash and no missing shader; sampled windows held about 16.7 ms, which does not measure whole races. Some audio underruns; the same `nvnDeviceGetInteger` and format 0x27 lines as on the Mac.
- **Mac** (Apple M4, macOS 26.6.2, Xcode 27.1, 2026-10-29): reaches the menu at 30 fps, before the 60 fps profile. 55 seconds under Metal API validation: 1,200 frames and the startup logos, no crash or missing shader, all 2,154 shaders compiled.

## What it needs

| Setting | Where | Why |
|---|---|---|
| The shaders of `SHADERS.BNDL` | `shader_split.py` `bnd2_shaders` | the 2,112 GLSLC outputs are in zlib resources of a Criterion Bundle 2 ([GLSLC outputs in Criterion's Bundle 2 resources](../../../docs/reference/shaders.md#glslc-outputs-in-criterions-bundle-2-resources)) |
| `fps_target 60`, `present_interval display` | [`game.tsv`](game.tsv), `hle_fpslock.c` | 60 fps (30 on the Switch), see [Frame rate](#frame-rate) |
| The frame limiter's spin cut (`main+0x4835a4`) | [`patches.py`](patches.py) | the engine's own 30 fps wait |
| `enc_thread on` | `game.tsv`, `nvn_rec.h` | the game waits on its fence every frame; see [Frame rate](#frame-rate) |

The rest of the profile is what `profile_guess.py` made of the survey: the 3.25 GB heap of the hardware pool, no virtual address memory.

The reader's [resource checks](../../../docs/reference/shaders.md#glslc-outputs-in-criterions-bundle-2-resources) change nothing on 1.0.3: over all 3,370 bundles (161 of them stored, the rest compressed; 518,261 blocks) the output is byte for byte that of the unchecked reader, nothing is rejected, and `SHADERS.BNDL` gives the same 2,112 GLSLC outputs.

## Frame rate

- **Current: 60 fps.** The engine has a 60 fps mode: a uint32 at `main+0xf0043c` (0 at boot) makes it take its frame time from the timer object at `[main+0xed96a8]` (+0x30 the frame in system ticks, +0x28 a tenth of it) and step the game by the floats at `[main+0xeffc60]` +0x30 (2D) and +0x50 (3D). All of these already hold 60 fps values when the game runs (320,000 ticks, 1/60 s): the host writes the mode switch (`hle_fpslock.c`, `MODE_U32`) and keeps the others at the target (`TICKS_U32`, `SECS_F32`; at 30 it writes 640,000 and 1/30). At 30 fps the frame limiter (`main+0x483530`) spins on the system tick (`main+0x86f070`) until `[x19+0x20]` ticks per frame have passed; the rewriter cuts its branch back (`main+0x4835a4`), and the host paces the presents (`present_interval display`; the game asks for interval 2). From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, `patches/010029B0118E8000/799D1061182C1302.yaml`; the frame-time instruction found by theboy181); each value was checked in the running game before writing.
- **Resolved: 18.1 ms frames (55 fps) in races at 60.** The game calls `nvnSyncWait` twice a frame on a fence it places after its submission, so with Metal encoding on the submit thread the frame was the game's work (about 4 ms) plus the host's encoding (2.5 ms) plus the GPU (6.3 ms) in series. A lower frame-tick target (66, 80, 120 fps) and handheld mode (GPU 5.4 ms) changed nothing; with `DM_NVN_ENC_THREAD=1` the race frames hold 16.66 ms (p95 16.9–17.5). The profile's `enc_thread on` makes that the Mac default for this game.
- Game speed at 60 was checked by play: races, the timer and the AI run at normal speed.

## Benchmark route

[`bench.tsv`](bench.tsv) contains `roadsters-reborn-boxster`, recorded with `DM_REC` from keyboard navigation and physical Flydigi Dune Fox driving input. It selects **Grand Ocean Coast → Roadsters Reborn**, the **Porsche Boxster Spyder**, and **GT Silver Metallic**. The event is on Grand Ocean Road at 3 PM. Its complete navigation and pause/restart prefix is retained; the measurement window is **540–600 seconds after launch**. The prefix includes stationary time outside that window.

Keep an untouched, local copy of the starting save. The tested state has Roadsters Reborn marked NEW, Grand Ocean Coast at 0/1, racer level 1 with zero bounty, and the Boxster Spyder available with 0.0 miles driven. Use English and the default controls. A different save can change the menu sequence: validate the screenshots before comparing timings. Saves are not part of this route or its report.

From the repository root, with `BUILD` set to the `010029B0118E8000_build` folder made by `--platform mac` and `START_SAVE` to that preserved save copy:

```sh
python scripts/bench.py "$BUILD" --route roadsters-reborn-boxster \
  --save "$START_SAVE" --runs 3 --fps 60 --scale 1 \
  --env DM_AUDIO=1 --env DM_LANG=en-US --env DM_WINDOW_TOP=1 \
  --env DM_KBD_PLAYER=own --env DM_PAD_MERGE=0 \
  --shots 600.5 \
  --lock "$BUILD/bench/measure.lock" --out "$BUILD/bench/m6-baseline"
```

Clear `DM_REC`, `DM_KEYS`, `DM_AUDIO_DUMP`, `DM_DETCLOCK`, and `DM_DETCLOCK_PACE` from the parent environment before replay. `DM_KBD_PLAYER=own` reserves the front end's first slot for the keyboard, but unsupported controller IDs can still be merged into player 1. Disconnect controllers or leave them untouched, and do not operate the game keyboard during replay. Keep the window visible. The benchmark copies the start save into each run's output. Review screenshots to confirm the race, car, motion and progress: a successful bench exit alone only proves that frames were produced.

For future timing runs, the command above requests visual proof at 600.5 s, after the 540–600 s measurement window. This explicit setting also works with the older benchmark script used below, whose default captured inside the window. [Upstream at review revision `a5281d0`](https://github.com/seedds/nxnative/blob/a5281d01796a283852229f103d36c93bc9ed04fd/scripts/bench.py#L103-L105) uses an after-window default. Keep any extra in-window diagnostic captures separate from timing comparisons; readback/PNG capture can stall a frame and change adaptive pacing. The after-window command has not been measured here.

- **Current: fixed-clock startup stalls at present 1.** On the tested M6 build, `DM_DETCLOCK=16666667` with `DM_DETCLOCK_PACE=1` left the guest main thread in `dm_guest_sleep_ns` after its first present. Normal guest time allowed loading to continue. This route therefore uses the normal clock. Frame-stamped input does not guarantee identical simulation or pictures across runs; see [input recording and its limits](../../../docs/development.md#recording-input-dm_rec).

### Initial Apple M6 results (2026-11-01, in-window screenshots)

**Current: near 60 fps, with occasional long frames.** Three sequential runs of the route on commit `ee6648cb2bb46c19255c1d04f88e9c40d36ef6b4`, using update 1.0.3 (v196608), main build `799d1061182c1302e08e706b84ce76e5`. Mac mini, Apple M6, 16 GiB, macOS 27.2 (26B123), Xcode 27.1 (27A9275); the locked Metal compiler is `32023.921 (metalfe-32023.921.6)`. Target 60 fps, render scale 1, audio on, normal guest time. The host logged a 1920×1080 swap chain/crop in a 1280×720 window drawable on a 144 Hz display, with Game Mode off. Other desktop apps were not closed; ambient temperature was not recorded.

`scripts/bench.py` at that commit measured each run's 540–600 s window. The actual trials explicitly set `--shots 95,185,305,450,525,555,585`, overriding the script's default: captures at 555 and 585 s were inside the measured interval. These original results include the capture workload and are not a measurement with screenshots excluded. No frames or capture-adjacent samples have been removed. Times below are milliseconds; `over_budget` counts frames longer than 25 ms (1.5 times the target frame time).

| Run | Frames | FPS | Frame P50 | Frame P95 | Frame P99 | Frame max | GPU P50 | GPU P95 | Dropped | over_budget |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3594 | 59.9 | 13.97 | 21.03 | 21.16 | 79.84 | 5.07 | 7.63 | 2 | 10 |
| 2 | 3587 | 59.8 | 13.99 | 21.06 | 21.21 | 103.35 | 5.11 | 7.53 | 2 | 11 |
| 3 | 3590 | 59.8 | 13.99 | 21.08 | 21.23 | 111.53 | 5.12 | 7.72 | 2 | 9 |

Median FPS is 59.8 (59.8–59.9), frame P99 21.21 ms (21.16–21.23), and GPU P50 5.11 ms (5.07–5.12). These are initial normal-clock results with diagnostic screenshots, not evidence of a constant 16.67 ms frame interval or an M4-to-M6 speedup. The causes of the 79.84–111.53 ms longest frames have not been established; these data alone do not attribute them to the GPU, shader compilation or CPU scheduling. The script did not report a numerical game-speed metric in this mode.

The largest frames occurred at 585.205, 599.871 and 595.534 s respectively. The first is near the nominal 585 s capture; the other two are away from capture times and each logged five new pipelines and ten shader libraries, with 100.9 and 116.0 ms of synchronous creation accounting. Those counters are per-frame differences, and their times need not equal the exclusive critical path. This is a diagnostic lead, not an attribution of every hitch. Screenshot requests/completions have no precise timestamp in these logs, and long-frame messages are printed eight frames late, so adjacent log lines alone do not establish capture causation.

Screenshots at 555 and 585 s in all three runs showed the silver Boxster driving on Grand Ocean Road, with no pause menu. Position and race time drifted: at 585 s the remaining distance was 6.8, 6.5 and 6.4 miles. The recorded route includes roadside/fence contact, so scene workload is not identical across runs. All three runs ended through the benchmark's graceful SIGTERM; each logged only the same three known red lines (device integers 0x60/0x61 and vertex format 0x27, noted below). This covers three approximately ten-minute launches, not one uninterrupted thirty-minute session. Private start saves, screenshots and raw logs are kept locally and are not part of this contribution.

## Rendering

- **Resolved: a crash at the first loading screen** after `texture upgraded to allow format views` (a 4096x1024 texture, base format 73, sampled as format 70): the upgrade's copy ended the render pass while the draw bound its arguments ([A texture upgrade while a draw binds its arguments ends the pass](../../../docs/reference/rendering.md#a-texture-upgrade-while-a-draw-binds-its-arguments-ends-the-pass)).
- **Current:** `vertex attribute format 0x27 (8 / 16-bit integer) feeds a float input`, converted on the CPU at replay; no visible effect.
- `nvnDeviceGetInteger` items 0x60 and 0x61 are answered 0, as before; no visible effect.

## Services

- Autolog, the game's online layer, finds no network (`nifm` answers as offline) and the game goes on to the career map after its connection screen.

## Found on this game

General changes first needed here; each applies to every game:

- [GLSLC outputs in Criterion's Bundle 2 resources](../../../docs/reference/shaders.md#glslc-outputs-in-criterions-bundle-2-resources) (`shader_split.py`)
- [A texture upgrade while a draw binds its arguments ends the pass](../../../docs/reference/rendering.md#a-texture-upgrade-while-a-draw-binds-its-arguments-ends-the-pass) (`nvn_mtl.m`)
- `hle_fpslock.c`: frame times in system ticks, frame steps in seconds and a mode switch as frame-limit fields; the profile key `enc_thread`
