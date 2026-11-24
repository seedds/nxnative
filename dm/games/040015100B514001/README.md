# Super Mario Bros. Wonder – Nintendo Switch 2 Edition + Meetup in Bellabel Park (`040015100B514001`)

A Switch 2 program of an application that also holds the Switch 1 program ([`010015100B514000`](../010015100B514000/README.md)). Build 1.2.1 on its title screen, SDK 21.4.1 built for the Switch 2 (`Ounce`), NVN2 shaders (`*.Nin_Ounce_NVN2`). Nintendo EPD's engine (sead, agl, gsys, Havok). Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x278000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB pool; the heap check (2026-11-14) found the first request did not fit and then a 4.94 GB heap, so `mem_reserve` holds it. These are the values the game was played with |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB of system resource |
| `modules main subsdk0 sdk` | `game.tsv` | the Switch 1 program has no `subsdk0` |
| `romfs_marker System/RegionLangMask.txt` | `game.tsv` | a file in Program 1's stub and merged RomFS layouts. The Mac packer checks the marker in that program's image |

No game-specific executable cuts and no `patches.py`: the rewriter rewrites `main`, `subsdk0` and `sdk` (`ec47e6f8846d2a7b6892e2682845c725`, `3fddebac0ad5c3f9e018dcadca9e55ed`, `31945f745022d0232d2562252641b3d3`), and the heap check found no address regions to handle (rewrite `4e7c0bdf9c75`). No `pad_layout`, so the virtual gamepad starts in the 2D layout.

## Files

- Packed from `Super Mario Bros Wonder - Nintendo Switch 2 Edition.nup` (8.84 GB) with `scripts/build_ipa_from_nsp.py`: this program, program 0's RomFS beside it (`romfs_programs/0.img`) and the upgrade add-on `010015100B515001` (5.26 GB) as DLC. [A Switch 2 Edition NUP stops at "2 base packages" or "is not a base title ID"](../../../docs/reference/packing.md#a-switch-2-edition-nup-stops-at-2-base-packages-or-is-not-a-base-title-id) says why the program mounts program 0's RomFS by index.
- Shaders: 30,629 in [`shader_map.tsv`](shader_map.tsv) (compute 338, pixel 18,718, vertex 11,573), from `Shader/` (11,431, filed as `other`), `Effect/` (10,184), `Lib/` (8,703), `Layout/` (271) and 40 in `main`. Both pair tables and the compute facts match #2957.
- A merged 1.2.1 NUP tested on 2026-11-19 has the same three program modules and puts `System/AddressTable` in Program 1's own RomFS. Two `SecredUber` archives in that image add 6,676 shader keys, so its 37,305-shader split fails the strict comparison against the registered tables. A read-only shader split omitting those two archives recovered all 30,630 keys from #2957 with the older reader. Main's BAHS reader removes the stray descriptor at `agl_technique_pfx.sharcb+0x10eda4`, and the refreshed map records the source of the duplicate shaders in `main` as the current reader picks them.
- [`pso_manifest.tsv`](pso_manifest.tsv) is empty: no prewarm list has been recorded yet (`DM_PSO_MANIFEST=off DM_PSO_RECORD`).

## Tested and open

- [PR #2957](https://github.com/seedds/nxnative/pull/2957) reports 50–60 fps on an M3 Pro (Mac15,6, macOS 27.2.1, 2026-11-14 to 2026-11-17). The author played the first levels and local co-op courses in Meetup in Bellabel Park with a Switch Pro Controller and an Xbox controller, saved and reloaded, and checked sound and movies. No environment overrides or game-specific executable cuts were needed.
- Merged NUP on M4 Pro (macOS 27.2.1, 2026-11-19): a build-folder profile with the merged shader tables built all 37,305 libraries on chain `690a7f403c92` and packed a 17.5 GB Mac app with the corrected marker. The app reached the title menu and committed its first test save. Bellabel Park is locked on a fresh save until first visited from the world map. [Nintendo's unlock requirements](https://www.nintendo.com/en-gb/Support/Purchases-Subscriptions/Games/Unable-to-Access-DLC-Super-Mario-Bros-Wonder-Nintendo-Switch-2-Edition-Meetup-in-Bellabel-Park--3066785.html)
- Open: a clean Mac app pack of the two-program NUP with the corrected marker, plus a repeat of local co-op and save/reload. Other multiplayer modes, mouse, camera, GameChat, and later worlds are untested. The 4.94 GB heap request has only been tested on Mac, and no IPA has been built.
- [`validated.tsv`](validated.tsv): the author ran `scripts/regress.py check --quick --record` on chain `b4355a0b78e1` (2026-11-17, Mac15,6, macOS 27.2.1). All 30,630 shaders built, with no changed MSL, new `[RED]` lines, crash or hang. The screenshots at 100 s had an RGB difference of 0.26. The quick run averaged about 29 fps without a prewarm archive.
- Route: [`smoke.tsv`](smoke.tsv) plays a `DM_REC` recording from an empty save: the first-boot screens (including the online tutorial) to the opening scene of the Flower Kingdom's castle, where the screenshot at 100 s is taken; it does not reach a level. The steps are frame numbers, so load time does not move them, and the last hold is stretched to 20,000 frames because a host whose scripted input ends exits by itself.

## Found on this game

- [Blocking inside `nvnWindowAcquireTexture` inflates a game's measured CPU frame time](../../../docs/reference/frame-rate.md#blocking-inside-nvnwindowacquiretexture-inflates-a-games-measured-cpu-frame-time): `sead::GameFrameworkNx` (`main+0x1bba10`, interval controller `main+0x1bbee0`, `waitDrawDone_` `main+0x184660`) calls `nvnWindowAcquireTexture` before recording draw work and measures CPU frame time before calling `nvnSyncWait(acquire_sync, -1)` while a presentation thread (`main+0xc5900`) submits and presents in parallel. Deferring acquire pacing and previous-present ticket waits onto the returned `NVNsync` removes the vsync wait from the game's measured CPU time without a game-specific frame-step stub. Open: live gameplay retest on Switch 2 Edition to confirm 60 fps recovery after an interval-2 frame.
