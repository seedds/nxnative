# Cult of the Lamb (`01002E7016C46000`)

The base game, 1.0.0 (v0). Unity 2019.4.35f1 IL2CPP with FMOD, SDK 13.3.2, NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays on the Mac at 60 fps (30 on the Switch). No IPA has been built; iOS is untested.

## What it needs

The profile the NSP script's survey guessed, the depth mode, the present interval and three cuts of its `sdk` build in [`dm/tools/sdk_patches.py`](../../tools/sdk_patches.py): no `patches.py`, no host change.

- **Resolved: registering the profile stopped the game in `nninitStartup`.** While the profile was provisional, `sdk_patches.py`'s SDK-wide `MATCH_CUTS` gave its `sdk` (357efc19…) the node area cut (`AddressRegionManager node area 1 GB → 62 MB`, implementation 3 of 5, at `sdk+0x37bd30` and `+0x37bdcc`) and the large-region alignment cut (`AllocateAddressRegion: regions of 1 GB and more aligned to 32 MB`, implementation 2 of 3, at `sdk+0x37bf2c`). Those skip a game's registered builds, so with this profile in `dm/games` the cuts went and `StandardAllocator::Initialize(nullptr, pool − used)` (returning to `main+0x3d9310`) aborted at once: `Break#1 ... svc at sdk+0x3ccf40`, from `sdk+0x32476c`. The same three cuts are now registered under its build ID, as Patrick's Parabox's (SDK 13.3.7), and it reserves its 3.14 GB at alias +0.06 GB again.

| Setting | Where | Why |
|---|---|---|
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000` | [`game.tsv`](game.tsv) | virtual address memory: the NPDM declares 16 MB of system resource and `main` never sets a heap (the heap check saw none in 5 s) |
| `app_pool 0xCD500000` | `game.tsv` | the survey's value; in its first 15 s in `nnMain` the game's regions reach alias +3.79 GB, within the 4.38 GB alias region; about 2.8 GB resident in play |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM's 16 MB system resource |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (`depth mode near=0 (as D3D / Metal)`, `main0_z` vertex entries); the pipeline bases made before it runs are made for that mode |
| `present_interval display` | `game.tsv` | see [Frame rate](#frame-rate) |

## Files and shaders

- RomFS: 90 files, 0.71 GB. 47 UnityFS `.bundle` files and `Data/data.unity3d` (0.44 GB), 28 FMOD `.bank` files (0.24 GB), three `.mp4` movies and the player's `Data/*.resource` file.
- The split finds 22,648 stage shaders: 12,407 vertex, 10,185 pixel, 55 compute and one geometry shader. 22,637 come from the Shader objects in the UnityFS bundles and `data.unity3d` (Unity 2019.4.35f1, read by `unityfs.py` with no change) and 11 from the GLSLC outputs in `main`. All 22,648 build for the Mac. `c1_guard.py` lists 5 whose c1 reads are not proven bounded.
- Movies: the Devolver Digital logo is 1920x1080 H.264 (High profile), decoded by VideoToolbox.
- A first `--platform mac` build on an M2 Max takes about 12 minutes. The shader stage takes 8 of them, the pipeline prebuild half a minute.

## Frame rate

The game asks for present interval 2 (30 fps), and with the default `present_interval game` it is paced at 30 fps: frame time p50 33.34 ms, p95 34.10 ms, with about 2 ms of GPU work a frame. Played that way, it felt slow and input lagged.

It steps by Unity's frame time, not by frames, as Monster Train 2 does, so `present_interval display` (one frame per display tick) plays at 60 fps at normal speed. Played windowed with a controller from a new save to the cult base, the game's speed looked normal. In play: frame time p50 16.67 ms, p95 about 17 ms, 0 to 1 dropped frames in each 300-frame window, GPU p50 about 3.6 ms, about 200 draws a frame. `DM_PRESENT_INTERVAL=game` restores the Switch's 30 fps.

Long frames seen in play are scene changes, not rendering: at a transition, two frames of 485 ms and 257 ms with 0.6 ms of GPU work each, while Unity's `BatchDeleteObjects` thread was busy.

## Memory

About 2.8 GB resident and 0.8 to 1.0 GB of Metal allocations in play, steady over 5,000 frames.

## Mac testing

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`; its final line runs the game with the correct paths (`DM_WINDOW=1` for a window with a controller, `DM_AUDIO=1` for sound). The title waits for a button press.

## Testing

- 2026-10-27, Mac (M2 Max, macOS 27.0.1), `main` d0b7c84, `--platform mac` with this profile as the survey guessed it:
  - **Headless, no audio:** the boot, the Devolver Digital movie and the title (`Ⓐ To Start`) at 30 fps, no red line.
  - **Windowed, with sound and a DualSense (Bluetooth), `DM_PRESENT_INTERVAL=display`:** from a new save through the opening to the cult base at 60 fps, with the game's speed looking normal; save commits. No red line, no abort. FMOD's output reported no underruns.

- 2026-10-27, Mac, this profile registered with its `sdk` cuts, on `main` 75f63ab, `--platform mac`, headless, no audio: the boot, the Devolver Digital movie and the title at 60 fps from the profile's `present_interval` (frame time p50 16.66 ms), no red line. The split gives the same 22,648 shaders, all from the cache. From a copy of the windowed run's save with scripted A presses (`DM_KEYS`), Continue loads into the cult base (the lamb in the ritual circle, the "Indoctrinate New Follower" objective).

## Open

- Input latency: the display link reports `game enqueue to target display` p50 about 81 ms, with submissions about 50 ms (three ticks) ahead of their target display. `DM_PRESENT_FIFO=0` (the newest queued frame each tick) is untested here.
- The six DLC packs are untested.
- A long play session; no `smoke.tsv` route yet.
- No prewarm list (`pso_manifest.tsv` is empty).
- iOS: no IPA built, untested.
