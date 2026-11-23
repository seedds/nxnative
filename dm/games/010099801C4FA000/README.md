# METAL GEAR SOLID: Peace Walker - Master Collection Version (`010099801C4FA000`)

Update 1.3.0 (v131072), Western/Global release. One game of two programs, packed as one, as on the cartridge: one NSP script run makes one IPA, one Mac app or one Mac run command, and the Unity launcher starts Peace Walker HD Edition through `oe::ExecuteProgram`.

| Program | What it is | Here |
|---|---|---|
| 0, the launcher (this profile) | Master Collection launcher (Game Selection, Master Book, Screenplay Book, Options): Unity 6000.1.2f1 IL2CPP, FMOD, `subsdk0` (`NintendoSDK_movie`) on NVN, SDK 19.3.11 | runs (60 fps) |
| 1, [Peace Walker HD Edition](../010099801C4FA001/README.md) | Konami's ported PSP/HD Edition engine on NVN, `subsdk0` (`NintendoSDK_movie`), explicit CPU / manual `nn::audio` AudioRenderer, SDK 19.3.11 | runs (30 fps) |

Tested on the Mac: disclaimers, title screen, main menu, Language Selection, and launching Program 1 through `oe::ExecuteProgram(1, 74 bytes)` (60.0 fps, 0 `[RED]` lines, 0 audio underruns in steady-state `DM_AUDIO=1` callbacks). The iPhone path compiles and packs; it has not run on a device.

## How it is one game

- **Profiles.** `programs 1=010099801C4FA001` links Program 1 ([010099801C4FA001](../010099801C4FA001/README.md)), which sets `program_of 010099801C4FA000` and has no separate row in the games table.
- **Launch parameters.** Selecting a language in Game Selection calls `oe::ExecuteProgram(1, ...)` with a 74-byte ASCII parameter string (`-region eu -lan en -selfregion EU -resolution 0 -upscale 0 -launcherpath 0` for English). Program 1's `launch_param` supplies the same bytes for standalone runs.
- **Saves.** One shared save folder across both programs, as on Horizon.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | NPDM declares 16 MB of system resource (virtual address memory) and Unity maps its regions directly; reaches alias +0.84 GB at boot |
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `subsdk0` (`NintendoSDK_movie`) for video playback |
| `depth_mode zero` | [`game.tsv`](game.tsv) | calls `nvnDeviceSetDepthMode` with near = 0 |
| `programs 1=010099801C4FA001` | [`game.tsv`](game.tsv) | packs Program 1 with the launcher |

No rewriter patch for Program 0. Program 1 carries a function-matched synchronization stub ([010099801C4FA001/patches.py](../010099801C4FA001/patches.py)).

## Mac input

With `DM_CTL`: wait ~22 s past the two disclaimer screens, then `A` on the title artwork → main menu (`Game Selection` highlighted) → `A` → Language Selection (`English` highlighted) → `A` launches Program 1.

## Open

- iPhone: no device run yet.
- Japanese release (`0100C6F01C4F8000` / `0100C6F01C4F8001`, reported in [issue #3212](https://github.com/seedds/nxnative/issues/3212)): has a different Program 0 `main` build (`9995809d…`) and requires the Multilingual Pack DLC for Program 1, though Program 1's `main` (`bca1a793…`), `subsdk0`, and `sdk` builds are identical.
- Master Book, Screenplay Book, and Return to Launcher from Program 1 were not exercised yet.
