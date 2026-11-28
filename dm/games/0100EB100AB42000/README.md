# FINAL FANTASY XII THE ZODIAC AGE (`0100EB100AB42000`)

Update 1.1.0 (`v65536`), `main` build ID `c2932c4d1c84ed7def7ceb1042b78951`, SDK 6.4.0. Square Enix's PhyreEngine port with the PS2 game's VU0 code emulated (`PS2 VU0 Assembly emulation initilization completed!` at boot), NVN. Modules `main`, `subsdk0`, `sdk`.

**Current:** plays on the Mac from the logos through the title, menus, field, dialogue with button icons, battles and cutscenes, at the game's 30 fps (p99 frame interval about 33 ms on an M3 Max). 6,454 shaders, all of them translated. Plays the same on an iPhone Duo.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | [`game.tsv`](game.tsv) | The game sets `nvnDeviceSetDepthMode` near = 0 (`[nvn] depth mode near=0`); with the default `gl` the bundled pipeline archive was built for near = -w and every base was built again on the device |
| `.vbf` stored in the bundle | `game.tsv` `zip_store` | `FFXII_TZA.vbf` (12.6 GB) and `FFXII_TZA_PATCH.vbf` are read by offset |

The other keys are the provisional profile's guesses, which held.

## Files and shaders

- RomFS: `FFXII_TZA.vbf` (93,491 records, 78,083 files), `FFXII_TZA_PATCH.vbf` (3 files, among them `phyredefaultlitshader.fx#….phyre`) and 72 loose `gamedata/nvn/branch_shader/*.fpo` programs.
- The archives are Virtuos `SRYK` as in [FFX/X-2](../0100BC300CB48000/README.md), but their compressed blocks are raw LZ4, not zlib, and `main` holds 23 Phyre programs outside any container ([LZ4 blocks in FFXII's VBF archives and Phyre programs in main](../../../docs/reference/shaders.md#lz4-blocks-in-ffxiis-vbf-archives-and-phyre-programs-in-main)). Before both, the split found 3,100 shaders and the game showed a black screen after the logos with sound and input working.
- Tables: 6,454 shaders (5,999 pixel, 371 vertex, 84 geometry), 3,051 from `main`, 3,403 from the archives and loose files; 742 variation pairs. Mac translation: none failed.
- **Resolved: duplicate representative labels (2026-11-18, same cause as [issue #2946](https://github.com/seedds/nxnative/issues/2946)).** Since `8d62601f` the split of the registered v0 + v65536 files stopped at the shader split, with 2,300 `shader_map.tsv` rows differing on each side. A fresh full split keeps all 6,454 map rows with the same multiset of package, stage and key, and every key the profile registered. All 2,300 moved rows are `ps_main.bin` Phyre programs from the `main` block at `0x1009159` (`#TFXO@` labels): 2,188 change only their package index, 111 their index and source label, and 1 only its label. No non-`main` row moves, the header line is unchanged, and `shader_facts.tsv`, `shader_pairs.tsv` and `pso_pairs.tsv` already matched the split. The refreshed map passes the packer's strict comparison on main `e0c30043`, and all 6,454 Mac shaders build. The PR author packed the registered build on an Apple M2 Pro with macOS 27.2.1 and Xcode 27.2, then let it boot with no input for 2.5 minutes: 30 fps (p50 frame time 33.3 ms over the first 300 frames), no shader keys outside the map, and only the known `BeginRecording re-entered` red line. Duplicate controls and code were not compared byte for byte outside the split's own checked selection. Gameplay and iPhone validation remain open for this refresh. Shared mechanism: [The split shader tables differ from a game profile's](../../../docs/reference/packing.md#the-split-shader-tables-differ-from-a-game-profiles).

## Validation

- 2026-11-12, Apple M3 Max, macOS 27.3: logos, title, a new game's opening, menus with the cursor highlight, dialogue with button icons, a battle and cutscenes, about 30 minutes; no `not in the shader map` line, no `no dual-source entry point` line.
- 2026-11-13, iPhone Duo, iOS 27.3: A new game, gone through intro tutorial, including cutscenes, example limit break from Basch, into early parts of Rabanastre, after ebf75c9; no purple blocks, limit breaks drawn right.
- 2026-11-14: the split's `raw_shaders` of bee218d on this `main` gives the table's 23 of 23 main#PHYRE keys and labels.
- Tests: `test_vbf.py` (LZ4 and zlib blocks, Phyre programs in `main`), all `test_split_*`, `test_docs`.

## Found on this game

- [LZ4 blocks in FFXII's VBF archives and Phyre programs in main](../../../docs/reference/shaders.md#lz4-blocks-in-ffxiis-vbf-archives-and-phyre-programs-in-main).
- [Same-pass feedback read through a snapshot](../../../docs/reference/rendering.md#same-pass-feedback-read-through-a-snapshot), uniform-buffer handles: magenta strokes over every limit break, from the echo shader `e40adef79f6f29b7` sampling the overlay layer it blends into.
- [A written secondary blend source (NVN dual-source blending)](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending): 27 `/gamedata/nvn/branch_shader/*.fpo` pixel shaders (20 in menus, dialogue, cutscenes and battle; 7 more in limit breaks, PR #2736) led to moving dual-source blending into every pixel shader's `main0_u` in `<key>_bx`, replacing per-game `dual_want.tsv` lists.

## Open

- One texture showed a blue stripe across it, once; not traced.
- `[RED] BeginRecording re-entered` about once a frame for the whole run (5,009 lines in a session of about 5,000 frames), with no visible effect since the shader fixes; not traced.
- Not run: `regress.py` (the LZ4 path runs only for blocks that are not one whole zlib stream; the adjacent code area in `raw_shaders` only for control sections in `main` that nothing references; [FFX/X-2](../0100BC300CB48000/README.md)'s `main` was not checked for such pairs).
