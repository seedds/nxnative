# A Little to the Left (`0100354017668000`)

Update 3.6.0 (v1179648) over the base game, with both DLC (`0100354017669001` v262144 and `0100354017669002` v65536: the title lists Cupboards & Drawers and Seeing Stars). SDK 13.3.7, NVN API 55.15. Unity 2020.3.26f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays on the Mac at 60 fps, as on the Switch, from the title into the first level; 30 fps is the player's choice. A double-clickable Mac app is packed. On an iPhone the provisional profile crashed when Play loaded the first level; this profile's `stdalloc_cap` is the fix: checked on the Mac with the iPhone's alias region, and played on an iPhone at 30 fps. An earlier build that made about the same room with a smaller pool played on an iPhone through more than ten levels.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NSP script's layout for a game with a system resource; the pool is the Switch's |
| `stdalloc_cap 0x80000000` (2 GB) | `game.tsv` | StandardAllocator would take 3.14 GB of the iPhone's 4.375 GB alias region, and Unity's own regions after it ran out of room on Play: see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (the NSP script's heap check) |
| `fps_target 60` | `game.tsv` | the game's own rate; it makes the player's **Frame rate** offer 30 fps: see [Frame rate](#frame-rate) |

No code of the game is cut.

## Memory

- `nninitStartup` calls `StandardAllocator::Initialize(nullptr, 3.14 GB)` (returning to `main+0x339710`, the pool less what is used), which takes alias +0.06 to +3.20 GB; Unity's own regions are made after it.
- **Resolved: the first level did not load with the iPhone's alias region.** With the provisional profile (no cap), the game reached its title, and Play then ran the alias region's high-water mark up to +4.26 GB of 4.375 GB while 1.01 GB was mapped. On an iPhone 17 Pro (2026-11-11) the game crashed there writing `0x1fffe0` (`pc=main+0x82b7a8`), 2 MB − 0x20: likely a block written past a null region, as [Sonic Superstars](../01008F701C074000/README.md#memory) wrote `0xffffe0` (not read further here). On the Mac with `DM_ALIAS_SIZE=0x118000000` it stopped on an empty screen with the GPU idle (2026-11-14, main `4258c22a`).
- **Fix:** `stdalloc_cap 0x80000000` gives StandardAllocator 2 GB through the [host's allocator gate](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space) (`StandardAllocator::Initialize(nullptr, 3.14 GB) … 2.00 GB of address space, the profile's stdalloc_cap`), about twice what the game maps in its first level. With the iPhone's alias region on the Mac, Play then loads the first level and it runs for over a minute: the high-water mark ends at +3.51 GB with 1.26 GB mapped. The cap applies on the Mac too (the request is larger than the cap), so the Mac runs the iPhone's layout.
- **Current:** about 2.06 GB resident on the Mac in the first level.

## Files and shaders

- RomFS: 219 files, 1.27 GB, everything under `Data/` (192 UnityFS files, 0.83 GB).
- 4,063 shaders: 2,001 vertex, 2,005 pixel, 56 compute and one geometry shader. All built for the Mac with no failure, and no run met a shader missing from the map.
- The prewarm list (`pso_manifest.tsv`, 56 keys: 37 shaders and 19 render pipelines) was recorded from a run through the title into the first level (`DM_PSO_MANIFEST=off DM_PSO_RECORD`).

## Frame rate

- **Current:** 60 fps on an M5 Max (frame time p50 16.67 ms, p95 16.8 ms, 300-frame windows in the first level), GPU p50 1.3 ms, CPU about 25 to 37% of one core.
- The game sets Unity's `QualitySettings.vSyncCount` to 1. The host finds it and `Application.targetFrameRate` through the internal-call table ([Unity's vSyncCount and targetFrameRate](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table)): `vSyncCount at [main+0x4adce60] +0x30 + [+0x4c] * 0xa8 + 0x68`. `fps_target 60` changes nothing at 60 and gives the player's **Frame rate** its 30 fps choice: with `DM_FPS_TARGET=30` the log says `vSyncCount (level 5) 1 → 2 vsyncs`, and the frame time is 33.33 ms (p95 33.5 ms, GPU 2.3 ms).
- Unity steps the game by the time that passed: on 2026-11-11 (main `084f5aa5`, the provisional profile with `fps_target 30`) the same input route from a fresh save reached the same transitions at the same times at 30 and 60 fps.

## Audio

- One `AudioOut` (opened from `main+0x3393e0`). Over 4,000 callbacks of 240 frames each: 0 underruns, 4 blocks queued at each callback.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app`.
- From a fresh save, driven by `DM_CTL`: at the title, Up 8 times 1.5 s apart puts the cursor on Play and A starts it (an A before any direction did nothing); the first level, the crooked picture frame, is on screen within 20 s. Add `DM_ALIAS_SIZE=0x118000000` for the iPhone's alias region.
- In handheld mode (`DM_OPMODE=0`) the menus take touches: a tap on Settings opens it, a tap on Play starts the first level.

## Testing

- 2026-11-14, Mac (M5 Max, macOS 27.2.1), main `4258c22a` with this profile, the packed app (`A Little to the Left.app`, 3.3 GB, ad-hoc signed) from a fresh save: the title with both DLC listed, Play into the first level and a minute in it, with the iPhone's alias region; the same with `DM_FPS_TARGET=30` at the title; in handheld mode, touch taps on the title's Settings and Play. No red line in these runs.
- 2026-11-11, Mac, main `084f5aa5` with the provisional profile: the title and chapter 1 at 60 fps, 0 red lines.
- 2026-11-11, iPhone 17 Pro: the provisional profile's crash on Play ([Memory](#memory)).
- 2026-11-11 to 2026-11-14, iPhone 17 Pro: played by hand through more than ten levels, built from main `8fb5ffeb` with the provisional profile and `app_pool 0x80000000` in place of the cap (`StandardAllocator::Initialize` then asks for about 2 GB, so its region leaves Unity's regions about the same room), and `fps_target 30`.
- 2026-11-14, iPhone 17 Pro, iOS 27.2, main `4258c22a` with this profile: played by hand, with the player's **Frame rate** set to 30 fps.
- Not tested: later chapters, the Daily Tidy, the DLC levels, and a long session's memory.

## Open

- The prewarm list covers the title and the first level only.
- One scripted touch drag (`DM_CTL` `drag`, 1.5 s) on the first level's frame did not turn it; a touch held before it moves was not tried.

## Found on this game

- The Unity 2020.3 instance of [StandardAllocator's address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space) taken before Unity's own regions, fixed by `stdalloc_cap` as on Sonic Superstars.
