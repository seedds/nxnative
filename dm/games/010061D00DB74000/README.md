# Ori and the Blind Forest: Definitive Edition (`010061D00DB74000`)

Update 1.0.2 (v131072) over the base game, SDK 8.2.0. Moon Studios' game (published by Microsoft) on Unity 2018.4.1f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. It plays on the Mac at 60 fps, as on the Switch, from the NSP script's provisional profile with its keys unchanged and no rewriter cut. The one change it needed is general: the host now honours [present interval 0](../../../docs/reference/frame-rate.md#present-interval-0-waits-for-no-vsync), which the game's shader prewarm uses. Reached: the shader prewarm behind the first loading screen, the title, the main menu, the save slot and difficulty screens, the opening storm, Skip Prologue from the pause menu, and control in the forest (walking, jumping, damage from thorns). Not run on an iPhone or iPad.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NSP script's guess. The game sets no heap; `nninitStartup` hands the pool to StandardAllocator as one 3.14 GB region. It held in play; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0, so the pipeline bases made before it runs take that mode |
| present interval 0 without a vsync wait | the host (`hle_nvn.c`), for every game | the first loading screen took about 5 minutes without it; see [Startup](#startup) |

## Startup

- The first loading screen (Ori running in a pool of light) is Moon's `UberShaderPrewarmer`. It draws every shader variant once, so that none compiles in play. For that stretch the game sets Unity's `vSyncCount` to 0 (`the game sets present interval 1 → 0`) and back to 1 at the end (`0 → 1`, then the save mount and the title).
- `UpdateStream` (`main+0x625b40`) draws shaders until a per-frame time budget runs out. If it drew 2 or fewer, it waits 15 frames. Each draw waits for Unity's graphics worker. While the host held every acquire to a display tick, the worker spent about 80% of its time there, so the prewarm mostly managed one or two shaders per 16 frames. With the host taking interval 0 as no vsync wait, as Horizon does, the prewarm ends 12 s after boot instead of 187 s (pipeline archive already filled, Apple M6, 2026-11-10). The first boot with an empty archive took about 5 minutes before the fix.
- **Not this:** at first this looked like a hang. Every loader thread was asleep, and the [Unity 2018 ThreadedStreamBuffer rule](../../../docs/reference/threads.md#unitys-stream-buffer-handshake-loses-a-wake-up-on-apple-cores) matches none of its 4 functions in this build. See [Open](#open).
- Names: Il2CppDumper's `script.json` was not made. The method names came from the metadata (v24.1: 64,853 methods, 10,329 types) and `CodeRegistration.methodPointers` (60,755 entries; the table at `main+0x3c208e8`, its count just before it at `main+0x3ce35a8`). Unity's `Debug.Log` writes nothing in this build (`nn::diag::detail::PutImpl` is never called).

## Frame rate

- **60 fps, as on the Switch.** After the prewarm the game runs at present interval 1. Unity's `targetFrameRate` stays -1 (`main+0x3d25488`, found by [the internal-call table](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table)). `fps_target` is not set; 120 fps is not tried.
- **Measured** (windowed, Apple M6, 60 Hz screen, the forest after the prologue): frame time p50 16.66 / p95 17.2 ms, GPU about 4.5 ms a frame, CPU 60% of one core.
- During the prewarm (interval 0) frames take p50 9.7–14.5 ms.

## Memory

- **Current:** from the title into the forest, resident up to 3.0 GB, 1.49 GB of it GPU memory. The alias region was mapped up to +1.76 GB of 4.38 GB (the iPhone's size).
- Later areas are not measured.

## Files and shaders

- The RomFS has 181 files, 3.42 GB: `Data/data.unity3d` (one 1.62 GB UnityFS bundle with every scene and asset), 157 `.resource` files of FMOD sound data (1.42 GB), and `StreamingAssets` with the Theater's ten `switchConverted/*.mp4` and two `.wmv` logos no run opened.
- 11,053 shaders (5,520 vertex, 5,489 pixel, 43 compute, 1 geometry): 11,049 from `data.unity3d` and 4 GLSLC outputs in `main`. All built for the Mac in 3 minutes, none failed.
- `pso_manifest.tsv` is empty: no run recorded one. The prewarm covers the draws: a run whose pipeline archive is empty compiles its pipelines behind the loading screen.

## Sound

- With `DM_AUDIO=1` the game opens one AudioOut (2 channels, 240-frame device buffer) and runs with no underruns; playback was not listened to or dumped.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command.
- `DM_CTL` presses of 0.15 s are often missed on the menus; hold them 0.4 s (`A~0.4`).
- From a fresh save: the title 12 s after boot (with a filled pipeline archive). A on Press A, A on Start Game, A on slot 1 (difficulty Normal is preselected), A on Normal. The opening storm starts; Plus opens the pause menu, Down then A is Skip Prologue, and about 15 s later Ori stands on a log in the forest. To the right are thorns.
- Controls checked: A on menus, Plus, Down, the left stick, A to jump.

## Open

- **Unity's stream-buffer handshake:** this build has the same eight store-release/plain-load pairs as FINAL FANTASY CRYSTAL CHRONICLES (`main+0x1fdcf74`, `0x1fdd5f4`, `0x1fdd664`, `0x1fdd768`, `0x1fdd8f0`, `0x1fdd928`, `0x1fdd9d4`, `0x1fddd8c`), built with other registers and block order, so the engine rule's fingerprints miss. No boot froze without load-acquires there: about 20 boots, 4 of them beside 12 busy shell loops. The rule is not extended to this build until one does.
- No fight, ability, save point or area past the first screen of the forest is checked.
- Sound was not heard or dumped. The Theater's movies are not played.
- iPhone and iPad runtime validation; no IPA has been built.
- 120 fps is not tried.
- Input has gone only through `DM_CTL`, not a controller or the keyboard.
- The base game (1.0.0) alone was not played or registered; packing it takes a derived profile.

## Found on this game

- [Present interval 0 waits for no vsync](../../../docs/reference/frame-rate.md#present-interval-0-waits-for-no-vsync): the shader prewarm behind the first loading screen.
