# Super Mario Bros. Wonder – Nintendo Switch 2 Edition + Meetup in Bellabel Park (`040015100B514001`)

A Switch 2 program of an application that also holds the Switch 1 program ([`010015100B514000`](../010015100B514000/README.md)). Build 1.2.1 on its title screen, SDK 21.4.1 built for the Switch 2 (`Ounce`), NVN2 shaders (`*.Nin_Ounce_NVN2`). Nintendo EPD's engine (sead, agl, gsys, Havok). Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. **Plays on the Mac** at 50 to 60 fps (MacBook Pro, M3 Pro, macOS 27.2.1, tested 2026-11-14 to 2026-11-17, a Nintendo Switch Pro Controller and an Xbox controller): everything loads, the first levels play without hiccups, a save reloads, audio and movies work, and the add-on, Meetup in Bellabel Park, is playable. No glitches, crashes or hangs were seen, and no env switches or patches were needed. Mac only: the 4.94 GB heap request was not tried on the iPhone, and no IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x278000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB pool; the heap check (2026-11-14) found the first request did not fit and then a 4.94 GB heap, so `mem_reserve` holds it. These are the values the game was played with |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB of system resource |
| `modules main subsdk0 sdk` | `game.tsv` | the Switch 1 program has no `subsdk0` |
| `romfs_marker Model/DVCastleSiro.bfres.zs` | `game.tsv` | a file of this program's own RomFS |

No game-specific executable cuts and no `patches.py`: the rewriter rewrites `main`, `subsdk0` and `sdk` (`ec47e6f8846d2a7b6892e2682845c725`, `3fddebac0ad5c3f9e018dcadca9e55ed`, `31945f745022d0232d2562252641b3d3`), and the heap check found no address regions to handle (rewrite `4e7c0bdf9c75`). No `pad_layout`, so the virtual gamepad starts in the 2D layout.

## Files

- Packed from `Super Mario Bros Wonder - Nintendo Switch 2 Edition.nup` (8.84 GB) with `scripts/build_ipa_from_nsp.py`: this program, program 0's RomFS beside it (`romfs_programs/0.img`) and the upgrade add-on `010015100B515001` (5.26 GB) as DLC. [A Switch 2 Edition NUP stops at "2 base packages" or "is not a base title ID"](../../../docs/reference/packing.md#a-switch-2-edition-nup-stops-at-2-base-packages-or-is-not-a-base-title-id) says why the program mounts program 0's RomFS by index.
- Its own RomFS: 19,034 files, 3.47 GB (`Voice`, `Pack`, `Model`, `Sound`, `UI`, ...).
- Shaders: 30,630 in [`shader_map.tsv`](shader_map.tsv) (compute 338, pixel 18,718, vertex 11,574), from `Shader/` (11,431, filed as `other`), `Effect/` (10,184), `Lib/` (8,704), `Layout/` (271) and 40 in `main`; 21,462 variation pairs. The full Mac library builds.
- [`pso_manifest.tsv`](pso_manifest.tsv) is empty: no prewarm list has been recorded yet (`DM_PSO_MANIFEST=off DM_PSO_RECORD`).

## Tested and open

- Reached and working: boot, the title, the first levels, save and reload, sound and movies, the Meetup in Bellabel Park add-on.
- Not tested: local co-op and other multiplayer, the Switch 2-only features (mouse, camera, GameChat), the later worlds, the iPhone and iPad.
- [`validated.tsv`](validated.tsv): `scripts/regress.py check --quick --record` passed on chain `b4355a0b78e1` (2026-11-17, Mac15,6, macOS 27.2.1): 30,630 shaders built, no changed MSL, no new `[RED]` lines, no crash or hang, the 100 s screenshot identical between runs. The full mode (with the pipeline prebuild) has not been run. The quick run presents about 29 frames a second, since it has no prewarm archive, against 50 to 60 fps in play.
- Route: [`smoke.tsv`](smoke.tsv) plays a `DM_REC` recording from an empty save: the first-boot screens (including the online tutorial) to the opening scene of the Flower Kingdom's castle, where the screenshot at 100 s is taken; it does not reach a level. The steps are frame numbers, so load time does not move them, and the last hold is stretched to 20,000 frames because a host whose scripted input ends exits by itself.
