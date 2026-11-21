# Chrono Cross in CHRONO CROSS: THE RADICAL DREAMERS EDITION (`0100AC20128AC001`)

Program 1 of [CHRONO CROSS: THE RADICAL DREAMERS EDITION](../0100AC20128AC000/README.md), packed with it, update 1.0.2 (v131072, `main` `7a54fb69872aa45a58e58e80f8672f9a`), SDK 13.3.0, NVN. Modules `main` and `sdk`. RomFS 7.5 GB: the PlayStation data under `data/cdrom` and `data/cdrom2`, the remaster's art under `data/hd`, Bink 2 movies (`data/movie/*.bk2`), fonts and `glslc` shaders.

Tested on the Mac (Apple M3 Max): started from the selection screen; the soft reset (L + ZL + R + ZR + Plus + Minus) returns to it.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `program_of 0100AC20128AC000` | [`game.tsv`](game.tsv) | a program of the collection, packed with it |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | the survey's rules for a game with no system resource |
| `romfs_marker data/font/large_font_kb_70.spritefont` | `game.tsv` | a file only this program's RomFS has |

The selection screen passes no launch parameter, so a run of this program alone (`--program 1`) needs none.

## Open

- Started, not played through.
