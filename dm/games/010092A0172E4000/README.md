# It Takes Two (`010092A0172E4000`)

Update 1.0.3 (v196608) over the base game, SDK 13.3.5. Hazelight Studios, Unreal Engine 4 with AngelScript gameplay scripts (5,655 `.as` files under `Nuts/Script`), NVN, Wwise. Modules `main`, `subsdk0` (the SDK's movie module) and `sdk`. The two DLC packages (Friends Pass, Full Game) are licences with no content. Two-player only: local split-screen co-op.

Status: on the Mac, the boot (the Turn Me Up logo, the title), the first-run setup (language, EA's privacy policy and user agreement, accessibility, sound, usage sharing), the main menu, Play Local with two players (player 2 joins, both pick their characters), the opening cutscenes (about 6 minutes) and the first level of The Shed in split-screen play, both players moving; Continue from the save. 60 fps in play (30 on the Switch), about 3.7–4.2 GB resident and 1.3–1.9 GB of Metal allocations over 11 minutes, no red line. Tested on 2026-11-20 with sound off.

## What it needs

| Setting | Where | Why |
|---|---|---|
| A 64 GB alias region: `mem_reserve 0x1028000000`, `mem_alias 0x1000000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the NSP script's Unreal default ([dm/games/profile_guess.py](../profile_guess.py)): `nninitStartup` reserves a 62 GB engine region; the rewriter's engine-wide rule gives it what the alias region has room for ([Unreal's engine address region and its options](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options)). The address region check reached alias +62.06 GB in its first 15 s in `nnMain` |
| FMallocBinned3 constructor call cut, the host's allocator in its place | the rewriter's engine-wide rule (`dm/tools/ue_malloc.py`), no build ID | [Unreal's FMallocBinned3 replaced by the host allocator](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator); the rewrite log: `cuts 2 obs 2` in `main` |
| `sys_resource 0x5000000` | `game.tsv` | the NPDM declares 80 MB of system resource |
| `fps_target 60` | `game.tsv`, `hle_fpslock.c` | the host writes `rhi.SyncInterval` 2 → 1; see [Frame rate](#frame-rate) |
| `depth_mode zero` | `game.tsv` | the address region check boot saw `nvnDeviceSetDepthMode` with near = 0 |

## Frame rate

- **Current:** 60 fps in play (59.3–60.0 in the power lines' windows of 300 frames, The Shed). The game asks for present interval 2; with `fps_target 60` the host writes `rhi.SyncInterval` 1 on both threads ([Unreal's frame limit and present interval, found by name](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name)). `bUseFixedFrameRate` is off: the rewriter found `UEngine::FixedFrameRate` (`[main+0xacf7858]+0x7a8`, the flag bit 0x40 of `+0x7a4`), but the host logged no write, so the engine steps by the time that passed.
- **Checked 2026-11-20:** the same 2 s stick hold for both players from the level's start covers the same distance at 30 and at 60 (screenshots framed alike). Not checked: jumps, timed puzzles, cutscene length at 60; 120 not measured (no `fps_max`).

## Memory

- The heap check: the C library heap is 0.01 GB, the engine region 62 GB.
- From the boot into The Shed: 3.7–4.2 GB resident, 1.3–1.9 GB of Metal allocations, flat over 11 minutes.

## Files and shaders

- One pak, `Nuts/Content/Paks/Nuts-Switch.pak` (9.9 GB, the profile's `romfs_marker`); `Nuts/Script` holds the AngelScript sources and their caches.
- Shaders: 32,102 (vertex 5,080, pixel 24,770, compute 2,171, geometry 81), all built. 119 pixel shaders lose a bindless `imageBuffer` access in translation (`[RED] lost without failing`); no shader that reaches the screen on the route above showed a fault, and no shader was missing from the map.
- The offline pipeline archive (`pso_offline.py`) failed in the Mac app build (`applegpu-nt: error: cannot find private metadata at offset '8'`); the app builds its pipelines on first start. `pso_manifest.tsv` is still the provisional empty list.

## Input

- Two players, split screen. Play Local's Join is the game's own lobby: a press on player 2's npad joins, no controller applet was opened. The log shows the supported npads set to `0x1ff` (No1–No8 and Handheld), then `0x7`. Each Mac controller is its own player, or the keyboard is player 1 with `DM_KBD_PLAYER=own` and the first controller player 2.
- The bring-up drove player 2 with a local `DM_CTL` extension (`p2 <keys>`, a press on player 2's controller path), not part of this profile.

## Mac routes

- First start: A at the title, A through the setup pages, B (disable usage sharing) → main menu. Play Local → New Game (or Continue) → player 2 presses A to join → A (Proceed) → character select: player 1 left, player 2 right, both A. The opening cutscenes end in The Shed's first room.
- Later starts skip the setup pages (the save keeps them).

## Open

- Sound has not been listened to (every run had `DM_AUDIO=0`); Wwise starts (`audren: OpenAudioRenderer … voices 4`).
- Play past The Shed's first room; two physical controllers.
- First-use pipeline hitches (up to 200 ms) until a prewarm list is recorded.
- iPhone: not tested; the 64 GB alias region is the Mac bring-up's.
