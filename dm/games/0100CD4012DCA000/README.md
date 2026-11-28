# ROMANCE OF THE THREE KINGDOMS XIV with Power-Up Kit (Asia) (`0100CD4012DCA000`)

Update 1.1.1 (v786432) over the base game (v720896 = 1.1.0), with its 1 DLC (index 49, 1.16 GB: the expansion's `pk.key`, its movies and data). SDK 10.4.1. Koei Tecmo's KTGL engine on NVN. Modules `main`, `subsdk0` (the movie library, NintendoSDK_movie: the MP4 logo, opening and event movies) and `sdk`. This is the Asian release of the game the Western profile [ROMANCE OF THE THREE KINGDOMS XIV: Diplomacy and Strategy Expansion Pack](../0100ED7012DB2000/README.md) covers: the same engine, memory layout and shader set, with Simplified and Traditional Chinese assets in the `SC`/`TC` folders instead of the `EN` ones, and a newer build (v786432 over v720896; the Western profile is on v458752 = 1.0.10). Plays on the iPad at 30 fps, as on the Switch. Started from the NSP script's provisional profile, which needed no change: the game booted to its strategy map on the first run.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource (META +0x14 = 0): no virtual address memory; `nninitStartup` sets a heap of the pool − used memory (the same layout as the Western release) |
| `mem_used 0x5000000` (80 MB) | `game.tsv` | the rewritten modules take more than the default 64 MB, which `SetHeapSize` checks the request against (the NSP script's measure) |

No rewriter cuts, host rows or run-time keys.

## Files and shaders

- Age of Calamity's RDB layout ([01](../../../docs/agents/01-extract.md#compressed-romfs-content)): the indexes `0002/*.rdb` over `0002/*.rdb.bin*` files and `0002/data/0x<KTID>.file`, 7.6 GB of the 12.3 GB RomFS. Movies are MP4 in `0003`, `0003_SC` and `0003_TC`; scenarios (`.s14`) in `0004`, `0010_SC` and `0010_TC`; messages in `0001_SC` and `0001_TC`; the expansion's data in `0000`.
- Languages: Traditional Chinese by default, the game also carries Simplified Chinese (`zh-Hans` through `DM_LANG`). There is no English in this release.
- Shaders: 55,383 GLSLC outputs (51,342 pixel, 3,837 vertex, 202 compute, 2 geometry): 55,263 in the G2S containers of the RDB entries under `0002` and 120 in `main`; the same set as the Western release, whose note covers the pairing. The `SC`/`TC` groups hold the Chinese variants' containers, and every group is in each shader's cache key.

## Input

- `main` reads the touch screen (`GetTouchScreenState`, `GetGestureStates`) as well as the pads. Touch is auxiliary by design, not a UI mode: Koei Tecmo's FAQ lists what it cannot do on the Switch release — moving a scrollbar, list operations on some screens, numeric settings ([FAQ](https://support.gamecity.ne.jp/hc/ja/articles/41238833165465)) — and recommends the controller ([manual](https://www.gamecity.ne.jp/manual/PaSjK1n4/jp/2150.html)). List paging is the right stick's vertical axis.
- On the iPad a tap selects the visible page's row and the following taps land on the detail pane the focus moved to; that matches the hardware, whose trapped cursor also stays on the current page. No host change was made for it.

## Status on the iPad

- Installed from the shared shell on the iPad mini (A17 Pro) with its RomFS and its DLC (`Documents/games/0100CD4012DCA000/aoc/49.img`), which the host reports and mounts.
- Title, a new game and the strategy map run with no `[RED]` line; the DLC's add-on content is reported (`aoc: reporting 1 add-on contents {49}`).
- Not run on the Mac beyond the profile's `address regions` check (the game reached `nnMain` there).

## Frame rate

- 30 fps, the game's own cap (as on the Switch). The build installed on the iPad carried `fps_target 60` for the host's present pacing only, and its `main.cuts.tsv` holds no cut, so the game stepped its own 1/30.
- A fixed 60 is possible; see [Open](#open).

## Open

- **60 fps, not enabled.** The game keeps its frame-rate table in `main`'s ro segment at `main+0x1e47120` (index 0 holds 30, index 1 holds 60, u32) and reads it through an index at `main+0x27c16e0` (bss, zero-filled, no writer in text), so 81 sites pick 30; the clock step it adds is the ro float `main+0x1e3a484` = 1/30, and the frame-rate lock at `main+0x896c70` reads a runtime field (`[x22+0x30]`) that holds 30 whatever the table says. A fixed 60 is two data words (table[0] 30 → 60, step 1/30 → 1/60) plus the lock's load redirected to table[1] (`adrp x8, #0x1e47000; ldr w8, [x8, #0x124]`) — the form the Android port uses, from FPSLocker's config for this build. It is not in this profile because a profile `fps_target 60` also makes the settings' **30** choice available, and with the step and table fixed at 60 that choice would run the game at half speed; nothing here was frame-captured on the Mac either.
- The touch limits above are the game's own; a front end can synthesise stick presses from drags, but this profile does not need it.