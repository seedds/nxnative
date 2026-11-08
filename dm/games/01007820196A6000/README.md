# Red Dead Redemption (`01007820196A6000`)

Base game 1.0.0 and update 1.0.4 (v262144), SDK 15.3.2, on NVN (3,628 shaders over both builds, RAGE effects). Modules `main` and `sdk`. Rockstar's RAGE engine in Double Eleven's Switch port (the survey's Unreal marker is a false match). All of the RomFS but two Bink movies and one Double Eleven pack is in 134 Rockstar RPF6 archives (11 GB, encrypted tables). The profile is the NSP script's provisional one with `render_cond on` added ([What it needs](#what-it-needs)). Getting it to run needed a reader for its archives and effects in `shader_split.py` ([Found on this game](#found-on-this-game)); its second audio renderer, for the movies, plays through the host's support for several renderers. Both builds are registered; the NSP script packs 1.0.4 when the update is in the folder.

On the Mac (2026-11-07, headless, 5 minutes driven with `DM_KEYS`): language selection, the Rockstar Games logo movie, the title, the autosave notice, the difficulty menu, the opening train cutscene and control of John Marston at Armadillo Station with the HUD and the minimap, with no `[RED]` line. It runs at the game's 30 fps (frame time p50 33.3 ms, p95 34.0 ms, max 35.7 ms; GPU p50 10.6 ms). The sound was not listened to (headless runs). The unsigned IPA is built but not tested on an iPhone.

Update 1.0.4 on the Mac (2026-11-07, headless, NX2Apple 1.2.3's engine): 3,583 shaders, 45 of them changed from 1.0.0's (`rage_lightprepass` among them), then two agreements the update adds before the title (the license agreement and Rockstar's terms of service, each accepted with A only after scrolling to its end with the right stick), the title at 30 fps (p50 33.3 ms, p95 35.4 ms) and no `[RED]` line in 8 minutes. Played on the Mac from a save in Armadillo (2026-11-08, Apple M4, macOS 27.0.1): the saloon, the street and the walk to the horses at 30 fps (frame time p50 33.3 ms), with distant characters and the sky over the ridge drawn right since the two fixes in [Found on this game](#found-on-this-game).

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0xb0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout, which holds: `main` sets a heap of at most the pool − 64 MB − its 1207 MB of bss |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `mem_used 0x4ca4000` | `game.tsv` | 1.0.0's rewritten modules exceed the default 64 MB (1.0.4's fit in it) |
| `modules main sdk` | `game.tsv` | `main` imports only from `sdk`; the Bink movies are decoded by `main` |
| `render_cond on` | `game.tsv` | the game draws under `SetRenderEnableConditional` (its only `[RED]` line before the key); without it small black squares showed in the title scene and the opening cutscene |
| `shader_groups` (one per RomFS folder) | `game.tsv` | the provisional guess, kept |
| `zip_store .bk2:.pkdswitch:.rpf:.metallib` | `game.tsv` | the archives and movies are already compressed |

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
- 1.0.4: `20:A,40:RDown~30,72:A,76:RDown~60,138:A,142:RDown~60,204:A` gets through language selection and both agreements (accepted once; the save keeps them); the title comes about 300 s in on a first start.
- `DM_KEYS` drives 1.0.0: A through language selection (about 20 s in), the logo movie plays, then Plus at the title, A through the autosave notice and the difficulty menu; the train cutscene runs on its own into Armadillo.

## Found on this game

- Resolved: no shaders were found at first: the effects are inside encrypted RPF6 archives → [RAGE RPF6 archives and `rgxd` effects](../../../docs/reference/shaders.md#rage-rpf6-archives-and-rgxd-effects); the language selection's two shaders are in a Double Eleven pack → [Double Eleven `.pkdswitch` packs](../../../docs/reference/shaders.md#double-eleven-pkdswitch-packs).
- Resolved: presentation stopped after the Rockstar Games logo: the game opens a second audio renderer for its movies, and the host rendered only one → [A game with more than one renderer](../../../docs/reference/audio.md#a-game-with-more-than-one-renderer).

- Resolved: light square blocks, 8×8 pixels and flickering, in the sky over the distant ridge (1.0.0 and 1.0.4): the cloud fog `rdr2_clouds_fog` (`c0ab3c051753e36d`) sampled the depth attachment its pass had just written → fixed in [PR #1866](https://github.com/seedds/nxnative/pull/1866) (its reference entry: Depth sampled after a draw of its pass wrote it), not yet on `main`.
- Resolved: characters a few metres away drawn as their hats and props only (their shadows had bodies): the character occlusion queries (`rage::CharacterVisQueries`) never reset the samples-passed counter, every query read 0 and every character counted as hidden → [Samples-passed reports with no reset before them](../../../docs/reference/rendering.md#samples-passed-reports-with-no-reset-before-them).

Otherwise it runs on mechanisms found on other games, including SDK 15.3.2's NVN and services.

## Open

- iPhone: the IPA is built but not tested; the 2.5 GB footprint needs the Increased Memory Limit entitlement.
- Faint light-blue blocks on distant buildings at the horizon in Armadillo (1.0.0), and small dark blocks in distant foliage and on the ground in 1.0.4's title scenes: not looked at again since the depth fix ([Found on this game](#found-on-this-game)), which may be their cause too.
- The sound has not been listened to.
- The empty prewarm list ([Shaders](#shaders)).
- 1.0.4 beyond Armadillo, and on an iPhone.
- Only 1.0.0's opening was played: riding, gunfights, the wider map, a save and its reload are untested.
