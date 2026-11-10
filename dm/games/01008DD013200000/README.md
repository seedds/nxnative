# Ori and the Will of the Wisps (`01008DD013200000`)

Update 1.2.1 (v196608) over the base game, SDK 9.4.0. Moon Studios' game (published by Xbox Game Studios) on Unity 2018.4.24f1 IL2CPP with Wwise on NVN, with Unity's native graphics jobs on (`Data/boot.config`). Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. It plays on the Mac at 60 fps, as on the Switch, from the NSP script's provisional profile with `depth_mode zero` added. It needs one fix of the engine's code: a race in Unity's constant-buffer table crashed 3 of 7 boots, and lock stubs in [`patches.py`](patches.py) close it ([Startup](#startup)). Reached: the shader prewarm loading screen, the Moon Studios logo movie, the title, the main menu, the save slot and difficulty screens, the opening scene, control in Swallow's Nest (walking, jumping) and the pause and options menus. Not run on an iPhone or iPad.

## What it needs

| Setting | Where | Why |
|---|---|---|
| lock stubs around Unity's constant-buffer table API (main 1.2.1) | [`patches.py`](patches.py) `STUBS` | the main thread and the render thread change the table at once; see [Startup](#startup) |
| `sys_resource 0x0`, 4 GB guest layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `mem_used 0x9d6c000` | [`game.tsv`](game.tsv) | the NSP script's guess: no virtual address memory, so main sets a heap (3.05 GB of a 3.25 GB heap region). It held in play; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0, so the pipeline bases made before it runs take that mode. The provisional profile had no key, so the NSP script's prebuild made bases the game never used and it compiled its own behind the loading screen |

## Startup

- **The crash (resolved):** boot crashed in its first second of shader loading in 3 of 7 Mac runs, in a `std::map` copy (`main+0x3e528`) walking a node pointer that is no pointer (`0x7e600004755` twice, `0x3` once), on the main thread or `UnityPreload`. It is [Unity's constant-buffer table changed by two threads at once](../../../docs/reference/threads.md#unitys-constant-buffer-table-changed-by-two-threads-at-once): the shared GfxDevice (`+0x2f88` = 0) keeps the layouts in a vector at `+0x3068` and takes no lock; the main thread queues "set this constant buffer" (client slot `0x208`, `main+0xab7d0`, command `0x2737`) and then registers the layout on the real device (slot `0x210`, `main+0x34f80`), while `UnityGfxDeviceWorker` runs the command (`main+0x24de0`). Probes showed both inserting key `0x142007ed4f5d` 0.1 ms apart. With `gfx-enable-gfx-jobs=0` and `gfx-enable-native-gfx-jobs=0` in a patched copy of `boot.config` both threads still inserted, so turning graphics jobs off is no fix.
- **Who uses the table** (probes at every function of its API, 45 s of loading, all on the shared device): the main thread through the register (`main+0x34f80`, 5,069 calls) and the member lookup (`main+0x353c0`, 5,915); the render thread through the program-parameter apply per draw (`main+0x24550`, 148,069), the set (`main+0x24de0`, 74,548), the slot moves (`main+0x25110`, 121,913) and the value writes (`main+0x35510`, 93,552); inserts (`main+0x2d640`) on both. No graphics-job thread touched it in that stretch.
- **The fix:** the stubs run the registry's own recursive mutex (`[main+0x7e48960]`, which the graphics-job devices take in `main+0x2df18`) around every function of that API on the shared device: those above and the multi-set (`main+0x2d200`), the slot reset (`main+0x29330`) and the copy into a graphics-job device (`main+0x29120`). The first version locked only the insert, the set and the register (10 of 10 boots), which left the render thread's lookups and writes open to the main thread's inserts; with every function locked, 10 of 10 boots ran on and the render thread's CPU time in the same scenes stayed the same (about 58 ms/s in the opening, 67–73 in the forest; 58 and 70–73 before).
- **Unity's stream-buffer handshake:** the [engine rule](../../../docs/reference/threads.md#unitys-stream-buffer-handshake-loses-a-wake-up-on-apple-cores) matches none of its four functions in this build (`main.match.tsv`), as in Ori and the Blind Forest. No boot froze without it (about 30 boots).
- **The first boot** of a working folder with no pipeline archive waits about 3 minutes at a black screen while the NSP script's prebuild (26,600 pairs) runs; point `run/pso_archive` at the build's.
- **The loading screen** (Ori running in a pool of light) is Moon's shader prewarm, as in Blind Forest, but this build keeps present interval 1 through it: about 85 s at 60 fps with a filled archive, then the Moon Studios logo movie (`subsdk0`, 1920x1080) and the title about 2 minutes after boot. How long it takes on the Switch is not known.
- With the window not visible (behind other windows when the run started) the display link stalled and the loading screen ran at about 28 fps; with the window in front it runs at 60.

## Frame rate

- **60 fps, as on the Switch.** The game presents at interval 1; `fps_target` is not set.
- **Measured** (windowed, Apple M6, 60 Hz screen, Swallow's Nest): frame time p50 16.66 / p95 16.80 ms, GPU about 6 ms a frame, NVN submit and present about 3 ms, CPU 66–105% of one core. Behind the dense foliage just past the area title, one 5-second stretch was GPU-bound (GPU p50 18 ms, frame p50 18.4 ms).
- 120 fps is not tried. The rewriter finds Unity's `Application.targetFrameRate` (`main+0x74f8ad0`) through the internal-call table, but not the `vSyncCount` getter (`main+0x3b51a0`, `[RED] … not read` from `unity_fps.py`), so the host could not write that.

## Memory

- **Current:** from the title into Swallow's Nest, resident up to 3.46 GB, 1.17 GB of it GPU memory. The heap is 3.05 GB of the hardware pool; no alias region is used (no virtual address memory).
- Later areas are not measured. On an iPhone, 3.46 GB resident is above what earlier 4 GB-class ports held; not checked.

## Files and shaders

- The RomFS has 4,981 files, 2.83 GB: 308 UnityFS bundles (`data_1.unity3d` and the rest, 1.32 GB), 3,989 Wwise `.wem` streams and 124 `.bnk` banks under `Data/StreamingAssets` (1.44 GB), two `.mp4` logo movies and two `.wmv`.
- 52,638 shaders, from the UnityFS bundles and main's GLSLC outputs. All built for the Mac in 26 minutes, none failed.
- The NSP script's prebuild makes pipeline bases from 26,600 shader pairs in about 3 minutes on the Mac (15,584 built, 10 left to the draws, none failed).
- `pso_manifest.tsv` is empty: no run recorded one.

## Sound

- With `DM_AUDIO=1` the game opens one AudioOut (2 channels, 240-frame device buffer) and runs with no underruns; playback was not listened to or dumped.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command. Keep the window in front.
- `DM_CTL` presses of 0.4 s (`A~0.4`) work on the menus.
- From a fresh save: the title about 2 minutes after boot. A on Press A, A on Start Game, A on slot 1, A on Normal Mode (preselected). The opening scene plays for about 45 s, then Ori stands on a log; `LLeft` walks off it into Swallow's Nest. Plus opens the pause menu (Resume, Skip Prologue, Options, Exit); one Down press of 0.4 s moved two entries, to Options.
- Controls checked: A on menus, the left stick, A to jump, Plus.

## Open

- No fight, ability, save point or area past the first screens of Swallow's Nest is checked. Skip Prologue was not reached.
- The two-minute loading screen: whether the prewarm is paced by frames and how long it takes on the Switch.
- Sound was not heard or dumped.
- iPhone and iPad runtime validation (memory first); no IPA has been built.
- 120 fps is not tried.
- Input has gone only through `DM_CTL`, not a controller or the keyboard.
- The base game (1.0.0) alone was not played or registered; packing it takes a derived profile, which would not get the lock stubs (they are keyed by 1.2.1's main).

## Found on this game

- [Unity's constant-buffer table changed by two threads at once](../../../docs/reference/threads.md#unitys-constant-buffer-table-changed-by-two-threads-at-once): the boot crash in the first second of shader loading.
