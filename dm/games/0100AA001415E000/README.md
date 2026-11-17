# FINAL FANTASY VI (`0100AA001415E000`)

The Pixel Remaster, update 1.2.1_5 (v458752) over the base game. Unity 2020.3.37f1 IL2CPP, SDK 14.3.0 and NVN 55.15. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays on the Mac through the opening, Narshe movement, the first guard battle and victory, menus and a completed quick save. iPhone and iPad remain untested.

## What it needs

| Setting | Why |
|---|---|
| `sys_resource 0x1000000` | The NPDM declares 16 MB of system resource, enabling virtual address memory |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000` | A 5 GB guest layout with 4.38 GB available for alias regions on iOS |
| `app_pool 0x40000000` | The heap check selected a 1 GB pool after measuring alias use within 0.13 GB of the region's end with the 3.21 GB survey pool |
| `depth_mode zero` | The game sets near = 0; offline pipeline bases must use the same convention |

No game-specific code patches or frame-rate overrides. The existing function-matched SDK rules reduce the address-region node area and align large regions to 2 MB ([SDK address-region rules](../../../docs/reference/memory.md#the-sdks-address-region-node-area)). Its SDK and movie-module builds are shared with [FINAL FANTASY V](../0100AA201415C000/README.md).

## Memory

- **Resolved profile guess:** the 3.21 GB survey pool makes StandardAllocator reserve 3.14 GB of address regions, and the heap check reaches alias +4.25 GB of 4.38 GB without setting a heap. The existing heap-check rule selects a 1 GB pool to leave room for Unity's other reservations.
- **Current:** StandardAllocator then reserves 0.94 GB at alias +0.06 GB. The follow-up address check reaches +2.16 GB in its first 15 seconds in `nnMain`.
- The nine-minute gameplay run uses the final 1 GB pool and `DM_ALIAS_SIZE=0x118000000`, testing the iOS alias-region size on the Mac. Its logged address high-water reaches +2.76 GB, with no allocation or address-space failure. Resident memory settles at 1,897–1,898 MB and Metal allocations at 616 MB through frames 28,800–31,800 after the battle; there is no steady growth in that interval.
- Device memory pressure and a long session are untested.

## Files and shaders

- RomFS: 2,535 files, including 2,491 UnityFS bundles. `Data/globalgamemanagers` identifies Unity 2020.3.37f1.
- 3,753 unique shaders: 1,846 vertex, 1,850 pixel, 56 compute and one geometry shader; 3,742 from Unity resources and 11 GLSLC outputs from `main`. The split supplies 1,846 vertex/pixel pairs.
- All 3,753 libraries compiled for both Mac and iOS on chain `66eab9c9a6b4`, with no translation failure, unresolved binding or unfillable driver-bank read.
- The initial pipeline prebuild makes 1,833 bases from 1,846 pairs, with 13 deferred to their draws and zero failures. `pso_manifest.tsv` records 113 shader keys and 58 render pipelines from the opening, Narshe, first battle, menus and quick save. `shader_seen.tsv` lists those 113 shader keys; later areas build additional pipelines on first use.

## Frame rate and audio

- **Measured on Apple M6:** the game requests present interval 1 and runs at 60 fps. After the battle, 300-frame windows report frame time p50 16.67 ms / p95 16.73–16.81 ms, GPU p50 1.02 ms and CPU 30–42% of one core. During the battle/results sequence some windows have p95 17.6–18.1 ms. This is a bring-up measurement, with no performance-change baseline.
- The AudioOut log reports 240 frames per device callback and zero underruns through 106,000 callbacks. Sound content was not independently assessed.

## Mac routes

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app` for a Mac app, or omit `--platform` for an unsigned IPA.

- A advances the title and selects New Game. Plus opens the narration, dialogue and credits march's Skip prompt; wait for it to open, then Left and A select Yes (the default is No). The credits march leads into Narshe; A advances Wedge's dialogue, and Plus closes the controls tutorial.
- Move north along the central street until the two guards stop the party. A advances their dialogue into the battle. Y enables the game's auto-battle; it defeats both guards and reaches the results screen. A advances results and returns to the street.
- X opens the game menu. From Items, six Down presses select Quick Save; A opens its confirmation, and Left then A selects Yes. The game displays `Quick Save complete.`; A closes it. Narshe's normal Save option is disabled outside a save point.
- Agent run artifacts are in `<NSP folder>/0100AA001415E000-work/`; `save_ref` preserves the quick save after the first battle. The builder's live save is in `0100AA001415E000_build/run/save`.

## Testing and open work

- 2026-11-15, Mac (Apple M6): a nine-minute run with the final pool and iOS alias-region size covers the opening narration, cliff dialogue and credits march, Narshe movement, the first guard battle through victory/results, menus and a completed quick save. No red line or missing shader. The run ends at its nine-minute watchdog (`sig=14`, expected).
- Built `FINAL FANTASY VI.app` (about 1.7 GB, ad-hoc signature verified) and `FinalFantasyVI.ipa` (about 1.3 GB, unsigned). Gameplay testing used the development host; the packaged app was not separately launched, and the IPA has not been installed or run on a device.
- No other-game regression run: this addition changes only this game's profile, tables and notes, with no host, rewriter or shader-chain change (low regression risk).
- Open: loading the quick save, normal save points, later battles/maps, physical controllers, a long session and iOS device testing. Only scripted controls were exercised.

## Found on this game

- The existing [SDK address-region rules](../../../docs/reference/memory.md#the-sdks-address-region-node-area) and automatic heap-check pool sizing apply without new code.
