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
- **Rechecked 2026-11-15**, chain `cd731f39944d`, Apple M6, window and HUD, audio on: a fresh save reached the title, New Game, the beach and left-stick movement with no red lines or crash. The 30 s standing beach sample (80–110 s after launch) contained 1,799 frames, 60.0 fps; frame time p50 16.67 / p95 16.72 / p99 16.77 ms, GPU p50 5.05 / p95 6.29 ms. Two frame intervals exceeded 25 ms and one presentation was dropped. Screenshots were taken outside the measurement window. This is a compatibility check, not an A/B performance comparison.
- In a run with `DM_CTL` `shot`s, a long frame of 60–190 ms followed each shot; runs without shots had no long frame after the New Game load (100 s on the beach, with and without sound): [Long frames a test run makes itself](../../../docs/reference/frame-rate.md#long-frames-a-test-run-makes-itself). The collector's stops are short: 2–18 threads paused per collection, each pause under 1 µs.

## Memory

- **Current:** from the title through the beach and the plateau, resident up to 2.26 GB and 1.27 GB of GPU memory, flat over a 4-minute run. The alias region held up to 3.02 GB mapped of 4.38 GB (the iPhone's size).
- Later areas are not measured; 1.36 GB of the alias region is left.

**Allocator integration check, 2026-11-17 (PR #2843):** an isolated copy of the provisional profile was restored to the original Switch's `app_pool 0xCD500000` (3285 MiB), without an allocator cap. The real heap check on main `2925d7af` plus the PR reached alias +4.26 GB of 4.38 GB and set `stdalloc_cap 0x40000000`, preserving the hardware pool. The real address-region check kept that cap: StandardAllocator took 1 GB at alias +64 MB, with 0.34 GB mapped; all regions reached +2.24 GB in the first 15 s of `nnMain`. This exercises the same heap-check rule that originally reduced this game's pool.

A paired `scripts/bench.py` invocation then compared the old 1 GB pool with the hardware pool plus the cap. Both hosts were built from the same source, on Apple M6, macOS 27.2, Xcode 27.2, shader chain `5ea15f2c8240`. Both used `DM_ALIAS_SIZE=0x118000000`, `DM_FPS_TARGET=60`, visible windows with the Metal HUD, copied saves, `40:A` to load the starting beach, and a 90–110 s standing measurement window; screenshots followed the measurement. The registered profile is unchanged.

| Setting or measurement | Old 1 GB pool, no cap | Hardware pool, 1 GB cap |
|---|---|---|
| StandardAllocator region / mapped at the final snapshot | 960 / 415.25 MiB | 1024 / 415.50 MiB |
| All regions mapped / highest live region end | 1.505 / +3.244 GiB | 1.529 / +3.131 GiB |
| Frame time p50 / p95 / p99 | 16.67 / 16.74 / 17.02 ms | 16.67 / 16.72 / 16.76 ms |
| GPU p50 | 4.68 ms | 4.70 ms |
| Frames in 20 s / longest frame | 1124 / 446.96 ms | 1200 / 17.08 ms |
| Peak logged resident / GPU allocated | 2205 / 1285 MiB | 2174 / 1301 MiB |

Both reached the same beach scene with equivalent screenshots apart from animation, no red line, crash, window-cover event or address-region allocation failure. The cap run's log confirms `StandardAllocator::Initialize(nullptr, 3.14 GB)` was given 1 GB. The old run had eight frames over 25 ms; their cause is not isolated, so this single pair establishes compatibility rather than a speedup. The game-specific graphics-quality effect reported for Pokémon Friends is not checked by this game, and no iPhone run was made.

`scripts/regress.py check --games 0100DA801624E000` also passed against `2925d7af`, with the isolated hardware-pool-plus-cap profile and the iPhone alias limit on both revisions: all 35,753 shaders built, no changed MSL or pipeline-interface failure, both 120 s boots reached the beach, and no red line or window-cover event. Screenshots at 111 s differed by 1 dHash bit and 0.09 RGB; frame p50/p99 were 16.67/16.77 ms on both. The final route uses empty saves and `40:A`: the earlier resume-save comparison reached different scenes despite using the same host binary. Report: `<NSP folder>/0100DA801624E000-work/pr2843/regression-fresh/report.json`. One game was selected because it reproduces the exact heap-check trigger and exercises Unity's own regions beside StandardAllocator; this is a moderate-risk change to provisional profiles, with no host or shader-chain change.

## Files and shaders

- The RomFS has 10,553 files, 2.75 GB: 87 Unity serialized files (`.assets` and their `.resS` / `.ress`), 135 FMOD banks (0.71 GB), and under `Data/GI` 10,042 small files (`.neo`, `.caw`, `.ecm`, `.rgb`, `.vis`, `.pos`; 0.59 GB). No bundles, no DLC.
- 35,753 shaders (17,845 vertex, 17,849 pixel, 56 compute, 3 geometry): 35,742 from `Data/` and 11 GLSLC outputs in `main`. Full Mac and iOS builds passed on chain `cd731f39944d` (2026-11-15): none failed, no shaders lost without failing, every binding decoded.
- The Mac prebuild made pipeline bases from 17,847 shader pairs in 173.1 s on the Apple M6 (2026-11-15): 17,833 built, 14 left to the draws, none failed. A run whose working folder has no archive builds them while the game waits at boot; point it at the NSP script's `run/pso_archive`.
- `pso_manifest.tsv` is empty: no run recorded one.
- The opening sea is an opaque saturated blue with a bright shore line, as on the console: its draws are `Custom/WaterEdge` over the clear colour. `Shader Forge/TUNIC water` is not drawn there.

## Sound

- With `DM_AUDIO=1` the game opens one AudioOut (2 channels, 240-frame device buffer) and runs with no underruns.
- **Rechecked 2026-11-15:** the 112 s Mac run had no underruns. `DM_AUDIO_DUMP` captured 108.33 s of stereo 48 kHz signed 16-bit PCM with nonzero samples (peak 27,038); playback was not listened to.

## iOS build

- **Built 2026-11-15:** an unsigned `Tunic.ipa` from the base NSP and update 1.0.3, with the full iOS shader library on chain `cd731f39944d`. The app compiled successfully (264 source files); it targets iPhone and iPad on iOS 26.0 or later. Not installed or run on a device.
- The default build has no bundled pipeline archive; the device prepares its pipeline bases on its first start when it takes the Metal 4 path. The Mac's archive is not an iOS archive.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command.
- From a fresh save: the title within 40 s (first frame at about 8 s), A on New Game, a fly-over of about 20 s, then control on the beach.
- From the start spot: the stairs are up and to the left; `LDown+LLeft`, `LLeft` about 1 s each, then `LUp+LRight` climbs them. The plateau above has the first enemies.
- Controls checked: the left stick, A on the title, Plus for the pause menu (Return To Game, Options, Quit To Title Screen).

## Open

- No fight, no item pickup, no save point and no area past the first plateau are checked, at any frame rate.
- Sound was dumped but not heard.
- iPhone and iPad runtime validation, including first-start pipeline preparation and memory use.
- Input has gone only through `DM_KEYS` and `DM_CTL`, not a controller or the keyboard.
- The base game alone was not played or registered; packing it takes a derived profile.

## Found on this game

- [The heap check's 1 GB pool picked a game's low graphics](../../../docs/reference/memory.md#the-heap-checks-1-gb-pool-picked-a-games-low-graphics): Unity allocator integration checked with the hardware pool and the host's cap under the iPhone's alias limit ([Memory](#memory)); no TUNIC graphics-quality defect was observed.
- [The callback took the frame queued after the previous tick](../../../docs/reference/frame-rate.md#the-callback-took-the-frame-queued-after-the-previous-tick): the queue wait fixed by the late latch, the backoff of its drain found here (PR [#2757](https://github.com/seedds/nxnative/pull/2757)). On the beach at 60 fps in a window on an M6 with a 120 Hz screen, present to screen p50 40.6 → 24.0 ms with no more frames shown 33 ms; with four 19 ms frames every 5 s (a test switch), the latch without the backoff cost one frame shown 33 ms more per hitch and kept none of the gain, which the backoff ended (8-9 frames against 12 without the latch). At a 30 fps cap: unchanged.
