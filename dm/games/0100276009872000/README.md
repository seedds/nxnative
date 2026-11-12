# ŌKAMI HD (`0100276009872000`)

Base game 1.0.0 (v0), SDK 4.5.0, NVN (126 shaders, BNSH files). Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. Capcom's 2017 remaster of the PS2 game, with CRIWARE sound (ADX2 `.acf` / `.afs2`) and CRI USM movies. The standard host and rewriter run this build with no game-specific fix: no shader overrides, rewriter cuts or host changes. The profile is the NSP script's provisional one, unchanged apart from its names and `pad_layout` ([What it needs](#what-it-needs)). Only the base game is registered.

On the Mac (2026-11-06, M4 Pro): the logos, the Joy-Con notice, the intro movie, the attract demo, the title, save creation, New Game, the whole prologue scroll and its in-engine cutscenes (Kamiki, Nagi and Orochi, Sakuya and Issun), then first control of Amaterasu at the Konohana shrine, with movement, the camera and sound. It ran at the game's own 30 fps (frame time p50 33.33 ms, GPU p50 7 ms at 1080p) with no `[RED]` line, no missing shader and a footprint of about 1.7 GB. A 10-minute and a 21-minute run ended at their time limit or when the window closed, with no crash. The IPA is not built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 3.25 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout, which holds: `main` sets a heap of at most the pool − 64 MB (the game asks for 2 GB) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk0` is the SDK's movie library: the USM movies' H.264 goes through `movie::Decoder` (1920x1088), which the host decodes |
| `shader_groups /data/=data /shader/=shader` | `game.tsv` | 35 shaders are `.bnsh` files in RomFS, 91 are in `main` |
| `pad_layout full` | `game.tsv` | a third-person 3D game: the right stick turns the camera |

## Files and shaders

- RomFS: 5,744 files, 9.17 GB, under `data/` (`.dat`, `.bin`, `.dds`, `.idd`, CRIWARE `.afs2` / `.acf` / `.adx`) and 25 CRI USM movies (2.44 GB).
- **Current:** 126 shaders (compute 8, geometry 2, pixel 88, vertex 28), one BNSH per file, all translated for the Mac. The game sets near = 0 depth and an upper-left window origin.
- **Open:** the prewarm list (`pso_manifest.tsv`) is empty: the first title-screen frames spent about 200 ms making pipelines on the submit thread (one 99 ms frame).

## Input

- **Open:** in the prologue scroll the `+ skip` prompt blinks, but Plus (pressed, held for 2 s, or pressed while the text types) does not skip it; Minus does nothing either. Plus works on the title ("Begin Story"), so the button reaches the game; whether the skip needs something the host does not report is not known. A advances the text: about 13 minutes of A at three presses a second from New Game to first control.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game; with `DM_FOCUS_KEEP=1 DM_WINDOW_TOP=1` a host started from a background shell keeps presenting.
- From a fresh save: the logos and the intro movie start by themselves; Plus every 3 s from about 20 s skips to the title and starts the story; A answers the save question (Yes) and picks New Game; then A through the prologue.
- The save is in `OKAMI_HD_NX/` at the save root.

## Found on this game

It runs on mechanisms found on other games, SDK 4.5.0's NVN, movie library and services included ([DARK SOULS: REMASTERED](../01004AB00A260000/README.md) and [SteamWorld Heist: Ultimate Edition](../010016100A186000/README.md) share the SDK).

- **Pipeline archive keys name a library by its bytes** (resolved): which vertex library its prewarm took varied between starts, so 2 to 8 bases were compiled again at some starts; `a7537d4f1786cfda` and `b555f7058434e1fa` are one library: [rendering.md](../../../docs/reference/rendering.md#bases-of-byte-identical-libraries-compiled-again-at-every-start).

## Open

- iPhone: no IPA built yet.
- The prologue skip ([Input](#input)).
- The empty prewarm list ([Files and shaders](#files-and-shaders)).
- Only the start was played: combat, the Celestial Brush (touch and motion controls), later areas and the other languages are untested. The game runs at 30 fps, as on the Switch; no 60 fps cut has been looked for.
