# Red Dead Redemption (`01007820196A6000`)

Base game 1.0.0 and updates 1.0.4 (v262144) and 1.0.7 (v458752), SDK 15.3.2, on NVN (3,628 shaders over the builds, RAGE effects). 1.0.7 changes only the RomFS: its `main` and `sdk` are 1.0.4's, and so are its shaders. Modules `main` and `sdk`. Rockstar's RAGE engine in Double Eleven's Switch port (the survey's Unreal marker is a false match). All of the RomFS but two Bink movies and one Double Eleven pack is in 134 Rockstar RPF6 archives (11 GB, encrypted tables). The profile is the NSP script's provisional one with `render_cond on` added, and 60 fps (30 on the Switch) by `fps_target 60` with `present_interval display` ([Frame rate](#frame-rate)). Getting it to run needed a reader for its archives and effects in `shader_split.py` ([Found on this game](#found-on-this-game)); its second audio renderer, for the movies, plays through the host's support for several renderers. All three versions are registered; the NSP script packs 1.0.7 from the profile as it is (its shader tables are made from 1.0.0 and 1.0.7), and 1.0.4 from a profile derived from it.

On the Mac (2026-11-07, headless, 5 minutes driven with `DM_KEYS`): language selection, the Rockstar Games logo movie, the title, the autosave notice, the difficulty menu, the opening train cutscene and control of John Marston at Armadillo Station with the HUD and the minimap, with no `[RED]` line. It runs at the game's 30 fps (frame time p50 33.3 ms, p95 34.0 ms, max 35.7 ms; GPU p50 10.6 ms). The sound was not listened to (headless runs). The unsigned IPA is built but not tested on an iPhone.

Update 1.0.4 on the Mac (2026-11-07, headless, NX2Apple 1.2.3's engine): 3,583 shaders, 45 of them changed from 1.0.0's (`rage_lightprepass` among them), then two agreements the update adds before the title (the license agreement and Rockstar's terms of service, each accepted with A only after scrolling to its end with the right stick), the title at 30 fps (p50 33.3 ms, p95 35.4 ms) and no `[RED]` line in 8 minutes. Played on the Mac from a save in Armadillo (2026-11-08, Apple M4, macOS 27.0.1): the saloon, the street and the walk to the horses at 30 fps (frame time p50 33.3 ms), with distant characters and the sky over the ridge drawn right since the two fixes in [Found on this game](#found-on-this-game).

Update 1.0.7 on the Mac (2026-11-10, Apple M6 16 GB, macOS 27.2, main at 47773ed1, window with the HUD, a fresh save, 10 minutes): the NSP script's `--platform mac` build took 5 minutes (split 45 s, 3,628 shaders compiled in 75 s). Language selection, three documents before the title (the license agreement, the terms of service and the privacy policy; the 1.0.4 route below accepts them), a subtitles prompt (A), the logo movie, the title, the difficulty menu, a new save (`RDR2SAVE0.SAV` written), the autosave notice, the opening in Blackwater and the train's arrival at Armadillo Station, with no `[RED]` line. Frame time p50 33.4 ms, p95 34.2 ms (the game's 30 fps), GPU p50 7.7 ms. Control of John Marston was not reached in the 10 minutes.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0xb0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout, which holds: `main` sets a heap of at most the pool − 64 MB − its 1207 MB of bss |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `mem_used 0x4ca4000` | `game.tsv` | 1.0.0's rewritten modules exceed the default 64 MB (1.0.4's fit in it) |
| `modules main sdk` | `game.tsv` | `main` imports only from `sdk`; the Bink movies are decoded by `main` |
| `fps_target 60`, `present_interval display` | `game.tsv`, `hle_nvn.c` | 60 fps (30 on the Switch); the player's **Frame rate** picks 30 or 60 ([Frame rate](#frame-rate)) |
| `render_cond on` | `game.tsv` | the game draws under `SetRenderEnableConditional` (its only `[RED]` line before the key); without it small black squares showed in the title scene and the opening cutscene |
| `shader_groups` (one per RomFS folder) | `game.tsv` | the provisional guess, kept |
| `zip_store .bk2:.pkdswitch:.rpf:.metallib` | `game.tsv` | the archives and movies are already compressed |

## Frame rate

- **Current: 60 fps** (30 on the Switch), 30 with the player's **Frame rate** (`DM_FPS_TARGET=30`). The game asks for present interval 2 once (`main+0x38ea0`, before the language selection) and has no other limit: with presents at one a display tick (`present_interval display`) the language selection, the title scene and the opening ran at 16.67 ms frames on an Apple M6 (GPU p50 5.5 to 6.6 ms, 7.3 to 7.7 at 30). The cap is applied by the present interval alone ([A present interval that is the game's only limit, in display mode](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode)), so the profile names no address and the setting holds for every build.
- **Speed (2026-11-10, 1.0.7, the same presses from a save with no progress):** the opening cutscene, shot every 2 s from 2 to 92 s after A on Normal at 30 and at 60: each 30 fps shot matches a 60 fps shot 0 to 2 s earlier, the same lead from the first to the last (the 60 run started the cutscene a moment sooner); a game counting frames would gain a second a second. After control at Armadillo Station, 4 s of the left stick forward ends at the station's porch steps at both rates. Rockstar's update 1.03 for the PS4 release (Double Eleven's port too) added a 60 fps mode on the PS5.
- Not checked at 60: riding, gunfights, Bink movies after the logo, and the iPhone (where the 2.5 GB footprint and heat may not hold 60).

## Files

- RomFS: 138 files, 11.24 GB: 134 `.rpf` (25 at the root, 10.36 GB, among them `shaders.rpf`, `fragments.rpf` and `audio_switch.rpf`; 107 in `redemption/territory_swall/`), two `.bk2` movies, and `d11extra/langselect.pkdswitch` with its index `.pkiswitch`.
- The RPF6 archives' table key is in `main`'s data segment (1.0.0: `main+0x3757f88`). 1.0.4 has the same key, loaded as eight big-endian words from four constants by its key setup (`main+0x145d8c`). `dm/gfx/tools/rpf6.py` finds it by trying `main`'s windows on an archive's first entry; it is never printed or written to disk. `rpf6.py <main NSO> <file.rpf> [--extract <dir>]` lists or extracts an archive.

## Shaders

- **Current:** 3,628 distinct shaders over both builds (the profile's tables are their union; 1.0.4 changes 45 of `shaders.rpf`'s); 1.0.0 has 3,583, all translated for the Mac: 3,413 from the 131 RAGE effects (`.nvn`, `rgxd`) in `shaders.rpf` (2,089 vertex, 1,324 pixel), 168 from effects in `main`, and 2 from the language selection's Double Eleven pack. Each RPF6 archive is read through `rpf6.py` ([RAGE RPF6 archives and `rgxd` effects](../../../docs/reference/shaders.md#rage-rpf6-archives-and-rgxd-effects)).
- **Open:** the prewarm list (`pso_manifest.tsv`) is empty.

## Memory

- The process footprint rose from 1.9 GB to about 2.5 GB while the world loaded, then stayed flat. That is above an iPhone's default limit: it needs the Increased Memory Limit entitlement.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game; headless runs set `DM_ROMFS`, `DM_SAVE`, `DM_TEMP`, `DM_MSL_DIR` and `DM_NVN_SHMAP` as the script does.
- 1.0.4 and 1.0.7: `20:A,40:RDown~30,72:A,76:RDown~60,138:A,142:RDown~60,204:A` gets through language selection and both agreements (accepted once; the save keeps them); on 1.0.7 the presses accept three documents, then A at the subtitles prompt (about 215 s), and the logo movie leads to the title about 270 s in on a first start.
- `DM_KEYS` drives 1.0.0: A through language selection (about 20 s in), the logo movie plays, then Plus at the title, A through the autosave notice and the difficulty menu; the train cutscene runs on its own into Armadillo.

## Found on this game

- Resolved: no shaders were found at first: the effects are inside encrypted RPF6 archives → [RAGE RPF6 archives and `rgxd` effects](../../../docs/reference/shaders.md#rage-rpf6-archives-and-rgxd-effects); the language selection's two shaders are in a Double Eleven pack → [Double Eleven `.pkdswitch` packs](../../../docs/reference/shaders.md#double-eleven-pkdswitch-packs).
- Resolved: presentation stopped after the Rockstar Games logo: the game opens a second audio renderer for its movies, and the host rendered only one → [A game with more than one renderer](../../../docs/reference/audio.md#a-game-with-more-than-one-renderer).

- Resolved: light square blocks, 8×8 pixels and flickering, in the sky over the distant ridge (1.0.0 and 1.0.4): the cloud fog `rdr2_clouds_fog` (`c0ab3c051753e36d`) sampled the depth attachment its pass had just written → fixed in [PR #1866](https://github.com/seedds/nxnative/pull/1866), on `main` since 81906931 as half of one depth rule (its reference entry: Depth sampled after a draw of its pass wrote it). 1.0.7's arrival at Armadillo shows no blocks over the hills.
- Resolved: characters a few metres away drawn as their hats and props only (their shadows had bodies): the character occlusion queries (`rage::CharacterVisQueries`) never reset the samples-passed counter, every query read 0 and every character counted as hidden → [Samples-passed reports with no reset before them](../../../docs/reference/rendering.md#samples-passed-reports-with-no-reset-before-them).
- Resolved: 30 fps with spare time, the player's Frame rate without effect: the present interval 2 is the game's only limit, and the host applied a cap only with an engine limiter it writes → [A present interval that is the game's only limit, in display mode](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode).

Otherwise it runs on mechanisms found on other games, including SDK 15.3.2's NVN and services.

## Open

- iPhone: the IPA is built but not tested; the 2.5 GB footprint needs the Increased Memory Limit entitlement.
- Faint light-blue blocks on distant buildings at the horizon in Armadillo (1.0.0), and small dark blocks in distant foliage and on the ground in 1.0.4's title scenes: not looked at again since the depth fix ([Found on this game](#found-on-this-game)), which may be their cause too.
- The sound has not been listened to.
- The empty prewarm list ([Shaders](#shaders)).
- 1.0.4 and 1.0.7 beyond Armadillo, and on an iPhone.
- Only the opening and Armadillo were played: riding, gunfights, the wider map and reloading a save are untested (1.0.7 writes a new save).
