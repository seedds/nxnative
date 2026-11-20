# SUPER ROBOT WARS Y (`010063301BD50000`)

Update 1.4.1 (v720896) over the base game, SDK 18.3.5. Unity 2022.3.44f1 IL2CPP with CRIWARE on NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. Plays on the Mac: the first-run screens, the title and its attract battles, a new game, the first map with a battle played by hand, the rest of it and the start of a second map under Auto, a manual save, Continue on the autosave after a restart, and the start of the Anniversary Expansion. No game-specific rewriter cut and no host change of its own. iPhone and iPad validation remains open.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x400000` | [`game.tsv`](game.tsv) | the NPDM's 4 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | `game.tsv` | [Cult of the Lamb](../01002E7016C46000/README.md#memory)'s layout, the same Unity generation; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (log: `depth mode near=0`), so the pipeline bases made before it runs take the `main0_z` vertex entries |
| `shader_groups /Data/=data` | `game.tsv` | everything with shaders is under `Data/`: the player's serialized files and the `StreamingAssets` bundles |

## Frame rate

- 60 fps, as the game asks (present interval 1).
- Windowless, the fixed 60 Hz timer gives p99 frame intervals around 20 to 28 ms over 40 minutes of play and of attract battles, with single stretches higher around a scene load. With a window, five minutes of Auto battles from the save gave a median p99 of 24.5 ms, at 60.0 fps in the power lines. Compare frame times only between runs of the same kind: a run that shoots a PNG every 0.6 s reads about 45 ms.

## Memory

- `nninitStartup` hands application pool − used memory to `StandardAllocator::Initialize(nullptr, size)` as one address region (0.91 GB at alias +0.06 GB, returning to `main+0xbe7d60`) and never calls `SetMemoryHeapSize`. The NSP script's heap check chose the 1 GB pool, and every run below used it.
- The node area and the region alignment take the SDK-wide function-matched cuts (`sdk+0x13be4c` / `+0x13bee0` and `sdk+0x13c034` / `+0x13c03c` / `+0x13c040`, [implementation 4 of each](../../../dm/tools/sdk_patches.py)).
- **Current:** 2.73 GB resident and 1.27 GB of GPU memory at most over 40 minutes of the first maps, 2.18 GB and 0.95 GB over 40 minutes of attract battles, both windowless. No growth over either. The NSP script's run command reads 4.0 GB resident: it opens the 1.67 GB prebuilt pipeline archive, and at frame 300 it reads 2.95 GB against 1.29 GB without that archive.
- **The alias region is not close to full, though its mark says so:** over 40 minutes of attract battles the high-water mark reached +4.27 GB of 4.38 GB with at most 1.82 GB of live regions; over 40 minutes of the first maps it reached +4.25 GB with at most 2.01 GB mapped. That follows from how the SDK places regions ([An alias high-water mark near the top is not exhaustion](../../../docs/reference/memory.md#an-alias-high-water-mark-near-the-top-is-not-exhaustion)). With `DM_ALIAS_SIZE=0xC0000000` (3 GB) the end of the table stayed at +2.99 GB for about 10 minutes of attract battles, with no red line and no crash. Later chapters are not measured.

## Files and shaders

- The merged RomFS has 19,941 files, 12.88 GB: 19,159 UnityFS bundles and 515 CRI UTF files. The survey also reports an FMOD marker in `main`, but the RomFS has no FMOD bank.
- There are 19 CRI USM movies, 0.51 GB, with hashed names under `Data/StreamingAssets/aa/Switch` ([Open](#open)).
- Ten DLC images are reported as owned, mounted and read at boot (their catalogs and bundles). Three of them are licences with almost no content. The shader split reads their roughly 6,800 files, and no row of `shader_map.tsv` has a DLC as its first source.
- 27,140 shaders (13,523 vertex, 13,527 pixel, 39 compute, 25 hull, 25 domain, one geometry): 27,129 from `Data/` and the player's 11 GLSLC outputs in `main`. All built for the Mac with no failure.
- The prebuild makes 13,513 pipeline bases from 13,529 shader pairs on the Mac: none fail, 16 are left to the draws. On that archive, a first start's prewarm compiled 66 bases (2.1 s) and the next start none (74 from the archive); each run added one or two bases in play.
- `pso_manifest.tsv` has 240 lines, recorded along the route below.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command.
- The CRIWARE logo, then the title by about 25 s. Left alone, the title alternates with attract battles from many series, a good test of battle scenes and of memory.
- From a fresh save: A at the title, A on the terms of service and the privacy policy, A then Right and A to decline the data usage consent. That reaches the menu: A on Start, A on a mode, A on Yes, A on a character, five times Down to Game Start, A, A on Yes.
- **Hold B (`B~3`) to skip a scene.** A tap of B closes the tutorial popups, which A does not. One scene, reached through Continue, had Skip Scene greyed out and advanced only with A.
- On a map, a menu opens with A on an empty cell (End Phase) and Plus opens Save, Load, System and To Title Screen. The cursor moves one cell per press, on the map's own grid and not the screen's. A confirms nothing on a cell out of range.
- Minus turns Auto on: the game then plays its side and only dialogue and popups need a press. The second map started with Auto off.
- Attack: A on the unit, A on Move, a cell, A; Attack; a weapon; a target; A on Start Combat.
- Continue appears in the menu once a save exists and resumes the autosave; the menu's Load was not tried. Anniversary Expansion is the menu's last line: Yes to the preset file, then the same mode and character screens.

## Found on this game

- [An alias high-water mark near the top is not exhaustion](../../../docs/reference/memory.md#an-alias-high-water-mark-near-the-top-is-not-exhaustion).
- Existing [Function-matched cuts](../../../docs/reference/rewriter.md#function-matched-cuts-for-unregistered-builds) work on this `sdk` build without a row of its own.
- **Not faults (resolved):** the black beyond the map edges is in the game's own tutorial pictures. The grey wisps that come and go over the maps are a drifting cloud layer: they move across land and buildings between frames, and are absent from the same spot at other moments. Black between scenes lasts 0.6 s at most (400 frames over 248 s of Auto battles). The hard-edged black strip and block in the Enemy Phase band look authored: the next frame resolves into the banner's sliding text, and the same shapes came up in two transitions. None of this was compared with a Switch.

## Open

- **The CRI USM movies are not tested:** none of the 19 files was opened on any route here (the log's `DM_FS=1` file opens name none of them), so CRI Mana playback has not run on this game.
- iPhone and iPad runtime validation; no IPA has been built. Memory there is the first thing to watch.
- Sound is checked only in the log (one AudioOut, no underruns): nobody has listened to it. Input has gone only through `DM_CTL`, not a controller or the keyboard.
- Everything past the start of the second map and of the Anniversary Expansion, including the intermission menus.
- The base game alone was not played or registered; packing it takes a derived profile.
