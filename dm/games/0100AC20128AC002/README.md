# Radical Dreamers in CHRONO CROSS: THE RADICAL DREAMERS EDITION (`0100AC20128AC002`)

Program 2 of [CHRONO CROSS: THE RADICAL DREAMERS EDITION](../0100AC20128AC000/README.md), packed with it, update 1.0.2 (v131072, `main` `f1fb9740b859009e4a6a90ddd06cac7b`), SDK 13.3.0, NVN. Modules `main` and `sdk`. Its data is under `data/radicaldreamers` (the ROM image, text and translation tables) beside the shared fonts and `glslc` shaders.

Tested on the Mac (Apple M3 Max): started from the selection screen; the soft reset (L + ZL + R + ZR + Plus + Minus) returns to it.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `program_of 0100AC20128AC000` | [`game.tsv`](game.tsv) | a program of the collection, packed with it |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | the survey's rules for a game with no system resource |
| `romfs_marker data/radicaldreamers/translate.txt` | `game.tsv` | a file only this program's RomFS has |

The selection screen passes no launch parameter, so a run of this program alone (`--program 2`) needs none.

## Open

- Started, not played through.
