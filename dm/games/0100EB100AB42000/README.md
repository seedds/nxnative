# FINAL FANTASY XII THE ZODIAC AGE (`0100EB100AB42000`)

Update 1.1.0 (`v65536`), `main` build ID `c2932c4d1c84ed7def7ceb1042b78951`, SDK 6.4.0. Square Enix's PhyreEngine port with the PS2 game's VU0 code emulated (`PS2 VU0 Assembly emulation initilization completed!` at boot), NVN. Modules `main`, `subsdk0`, `sdk`.

**Current:** plays on the Mac from the logos through the title, menus, field, dialogue with button icons, battles and cutscenes, at the game's 30 fps (p99 frame interval about 33 ms on an M3 Max). 6,454 shaders, all of them translated. Not run on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | [`game.tsv`](game.tsv) | The game sets `nvnDeviceSetDepthMode` near = 0 (`[nvn] depth mode near=0`); with the default `gl` the bundled pipeline archive was built for near = -w and every base was built again on the device |
| [`dual_want.tsv`](dual_want.tsv) | profile | 20 pixel shaders blend with their output 1 as the second source. Without the dual-source entry point the second source counts as zero, and the menu cursor's highlight, button icons in dialogue and character icons drew nothing; most cutscene backgrounds and transitions were missing too. Listed with `DM_NVN_DUAL_WANT` over menus, dialogue, a cutscene and a battle |
| `.vbf` stored in the bundle | `game.tsv` `zip_store` | `FFXII_TZA.vbf` (12.6 GB) and `FFXII_TZA_PATCH.vbf` are read by offset |

The other keys are the provisional profile's guesses, which held.

## Files and shaders

- RomFS: `FFXII_TZA.vbf` (93,491 records, 78,083 files), `FFXII_TZA_PATCH.vbf` (3 files, among them `phyredefaultlitshader.fx#….phyre`) and 72 loose `gamedata/nvn/branch_shader/*.fpo` programs.
- The archives are Virtuos `SRYK` as in [FFX/X-2](../0100BC300CB48000/README.md), but their compressed blocks are raw LZ4, not zlib, and `main` holds 23 Phyre programs outside any container ([LZ4 blocks in FFXII's VBF archives and Phyre programs in main](../../../docs/reference/shaders.md#lz4-blocks-in-ffxiis-vbf-archives-and-phyre-programs-in-main)). Before both, the split found 3,100 shaders and the game showed a black screen after the logos with sound and input working.
- Tables: 6,454 shaders (5,999 pixel, 371 vertex, 84 geometry), 3,051 from `main`, 3,403 from the archives and loose files; 742 variation pairs. Mac translation: none failed.

## Validation

- 2026-11-12, Apple M3 Max, macOS 27.3: logos, title, a new game's opening, menus with the cursor highlight, dialogue with button icons, a battle and cutscenes, about 30 minutes; no `not in the shader map` line, no `no dual-source entry point` line.
- Tests: `test_vbf.py` (LZ4 and zlib blocks, Phyre programs in `main`), all `test_split_*`, `test_docs`.

## Found on this game

- [LZ4 blocks in FFXII's VBF archives and Phyre programs in main](../../../docs/reference/shaders.md#lz4-blocks-in-ffxiis-vbf-archives-and-phyre-programs-in-main).

## Open

- One texture showed a blue stripe across it, once; not traced.
- `[RED] BeginRecording re-entered` about once a frame for the whole run (5,009 lines in a session of about 5,000 frames), with no visible effect since the shader fixes; not traced.
- Not run: the iPhone, `regress.py` (the LZ4 path runs only for blocks that are not one whole zlib stream; `phyre_main` only for games with `.vbf` or `.phyre` files, which includes [FFX/X-2](../0100BC300CB48000/README.md), whose `main` was not checked for such pairs).
