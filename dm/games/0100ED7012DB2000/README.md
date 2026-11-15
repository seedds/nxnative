# ROMANCE OF THE THREE KINGDOMS XIV: Diplomacy and Strategy Expansion Pack (`0100ED7012DB2000`)

Update 1.0.10 (v458752) over the base game, with its 43 DLC (8 with content, the rest licences only). SDK 10.4.1. Koei Tecmo's KTGL engine on NVN. Modules `main`, `subsdk0` (the movie library, NintendoSDK_movie: the MP4 logo, opening and event movies) and `sdk`. Plays on the Mac; 30 fps, as on the Switch. Not run on the iPhone yet. Started from the NSP script's provisional profile, which needed no change: the game booted to its strategy map on the first run.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource (META +0x14 = 0): no virtual address memory; `nninitStartup` sets a heap of the pool − used memory |
| `mem_used 0x4600000` (70 MB) | `game.tsv` | the rewritten modules take more than the default 64 MB, which `SetHeapSize` checks the request against (the NSP script's measure) |

No rewriter cuts, host rows or run-time keys.

## Files and shaders

- Age of Calamity's RDB layout ([01](../../../docs/agents/01-extract.md#compressed-romfs-content)): the indexes `0002/*.rdb` over `0002/*.rdb.bin*` files and `0002/data/0x<KTID>.file`, 7 GB of the 12.3 GB RomFS. Movies are MP4 in `0003`, `0003_SC` and `0003_EN`; scenarios (`.s14`) in `0004` and `0010_EN`; messages in `0001_EN`; the expansion's data in `0000`.
- Shaders: 55,383 GLSLC outputs (51,342 pixel, 3,837 vertex, 202 compute, 2 geometry): 55,263 in the G2S containers of the RDB entries under `0002` and 120 in `main`. The vertex shaders are single-stage programs, paired with a cover pixel shader. All build for the Mac (1 h 53 min). The profile's other `shader_groups` hold none; they stay as the provisional profile guessed them because the groups are in every shader's cache key.

## Input

- `main` reads the touch screen (`GetTouchScreenState`, `GetGestureStates`) as well as the pads, a USB mouse and a keyboard. Touch has not been tried.
- `pad_layout` is the default (2D). Whether the strategy map wants the right stick or the D-pad on the iPhone has not been checked.

## Status on the Mac

- From the Koei Tecmo logo and the opening movie, the title, the main menu, Start New (tutorial declined), the scenario list (with the expansion's EP scenarios), officer and scenario settings, Sun Jian in 184's Yellow Turban Rebellion, the opening story and its events (skipped with Plus), to the strategy phase on the 3D hex map: terrain, forests, territory tint, the HUD, the advisor and the Force Commands menu, with no `[RED]` line.
- 30 fps, as on the Switch (frame time p50 33.2 ms, p95 35.4 ms; GPU 7 ms a frame on the map), sound with no underruns; about 2.7 GB resident on the map, GPU 1.8 GB.
- Not reached: the advancement phase. On the first turn X went to the advisor panel (each press showed its next suggestion), not to the phase button.

## Mac routes

- The work folder's `run/boot.sh <log> [secs] VAR=value…` runs `build/k14host` (a copy of a worktree's `dm/host/dmhost`); `run/ctl.sh press|snap|line` drives a run started with `DM_CTL=<run>/ctl.txt`; shots with `DM_NVN_SHOT_OUT=1280x720`.
- From a fresh save: A (logo), A (movie), A (title), A at the main menu after its load (about 75 s from boot), A on Start New, B (no tutorial), A, A (first scenario), X (officer settings), A (Sun Jian), X (scenario settings), X (confirm), then Plus and B through the opening story, X on the event card and Plus, X at the dialogue: the strategy map, about 3 minutes.

## Found on this game

General host changes first needed here; each applies to every game:

- **A deferred-release drain reading entries it had handed back** (`defer_drain`, a crash in `class_conformsToProtocol` from a completion handler in the main menu): [A deferred-release drain read entries it had handed back to the queue](../../../docs/reference/nvn-objects.md#a-deferred-release-drain-read-entries-it-had-handed-back-to-the-queue).

## Open

- The iPhone: no IPA built or run yet (the iOS shader libraries are not built).
- The advancement phase, battles and duels on the Mac.
- Touch and the iPhone pad layout (above).
