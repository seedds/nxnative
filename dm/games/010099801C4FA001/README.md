# METAL GEAR SOLID: Peace Walker - Master Collection Version, Program 1 (`010099801C4FA001`)

Program 1 of [METAL GEAR SOLID: Peace Walker - Master Collection Version](../010099801C4FA000/README.md), packed with it, update 1.3.0 (v131072), SDK 19.3.11. Modules `main` (`bca1a793…`), `subsdk0` (`NintendoSDK_movie`), and `sdk`.

Konami's ported PSP/HD Edition engine on NVN (104 shaders in `SHADER/NX/*.glslc`, 30 fps) with an explicit CPU / manual `nn::audio` AudioRenderer driven by the game's own `AudioRenderingThread`.

Tested on the Mac (30.0 fps): handoff from Program 0 and standalone boot through the loading map, Konami logo, title screen, main menu (`BACK TO MAIN MENU` / `NEW GAME` / `EXTRAS`), and New Game setup (`Input your player name` prompt), plus 10/10 CPU-loaded boot stress runs (`DM_PREEMPT=0` and `DM_PREEMPT=1`, `DM_AUDIO=1`) with zero crashes and zero `[RED]` lines. Steady-state audio (`DM_AUDIO=1`) runs at 0 underruns across 6,000 consecutive callbacks after initial boot loading.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `program_of 010099801C4FA000` | [`game.tsv`](game.tsv) | packed with Program 0 (the Unity launcher) |
| `launch_param hex:…` | [`game.tsv`](game.tsv) | 74-byte ASCII parameter string Program 0 passes via `oe::ExecuteProgram` (`-region eu -lan en -selfregion EU -resolution 0 -upscale 0 -launcherpath 0`), for standalone runs |
| 3.5 GB layout: `mem_reserve 0xe0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | NPDM declares 0 MB system resource; `main` has 625.8 MB BSS and sets a 2.53 GB heap |
| `depth_mode zero` | [`game.tsv`](game.tsv) | calls `nvnDeviceSetDepthMode` with near = 0 |
| `pad_layout full` | [`game.tsv`](game.tsv) | third-person stealth action with camera on the right stick |
| Render-thread synchronization | [`patches.py`](patches.py) | `MATCH_STUBS` (`game`-bound) fixes two data races in `kick_render` (`main+0x8ba40`) and `wait_render` (`main+0x8bbe4`); see [Render-thread synchronization](#render-thread-synchronization) |

## Render-thread synchronization

The renderer manager drives a dedicated `RenderThread` (core 2, priority 58, `main+0x8bca0`) with two manual-clear `nn::os` events: `+0x470` (kick, initialized 0) and `+0x480` (done, initialized 1). Two races in the game's own code trigger reliably on Apple Silicon (2/5 runs failed on `DM_PREEMPT=0` and 1/5 failed on `DM_PREEMPT=1` without the patch):

1. **Premature kick before `set_resolve` (`SIGSEGV` at `main+0xf28c`, `addr=0x20`).** `kick_render` (`main+0x8ba40`) signals `+0x470` at `+0x1c..+0x20`, waking `RenderThread` on core 2 ~90 instructions before calling `set_resolve` (`main+0x9dcc1c` at `+0x18c`) to populate the non-double-buffered MSAA resolve target `[renderer + 0x1210..0x1224]`, which `EndFrame` (`main+0x9dc694`) clears to `NULL` after every frame. Because producer (core 0) and `RenderThread` (core 2) run on different cores, `DM_PREEMPT=1` does not prevent this.
2. **Unwaited startup kick (`[RED] BeginRecording re-entered` / concurrent `WindowAcquireTexture`).** Startup calls `kick_render` twice (`main+0x1f4ae8`, then `main+0x1f4858`) with no `wait_render` between them while `+0x480` is already signaled (1). The first `wait_render` (`main+0x1f4864`) consumes the stale signal immediately, shifting `+0x480` one frame ahead so `main+0x1f5e28` returns while `RenderThread` is still inside frame 4; `main thread` then enters `BeginFrame`/`EndFrame` (`main+0x1f863c..+0x1f866c`) concurrently with `RenderThread`.

[`patches.py`](patches.py) restores happens-before ordering via `MATCH_STUBS` (`game`-bound, matched on `kick_render`, `wait_render`, `RenderThread`, and the three `nn::os` event PLT thunks):

- `wait_render` (`+0x60..+0x68`) returns after `WaitEvent(+0x480)` without clearing `+0x480`, maintaining the invariant `+0x480 == 1` iff `RenderThread` is idle.
- `kick_render` (`+0x18`) waits for `+0x480`, clears `+0x480`, stores `[mgr + 0x454]`, and skips the early `SignalEvent(+0x470)`.
- `kick_render` (`+0x190`, after `set_resolve`) signals `+0x470` through callee-saved `x23` (`mgr + 0x180`, so `[x23, #0x2f0] == mgr + 0x470`).

## Found on this game

- [Explicit CPU / manual renderer](../../../docs/reference/audio.md#explicit-cpu--manual-renderer): Program 1 opens `AudioRenderer` with `renderingDevice == 1` (CPU) and `executionMode == 1` (Manual) and drives `ExecuteAudioRendererRendering` from its own `AudioRenderingThread` (first reported in [issue #3212](https://github.com/seedds/nxnative/issues/3212)).

## Mac input

With `DM_CTL`: wait ~29 s for the title screen fade-in (`PRESS (+) BUTTON`), then `Plus~0.25` → main menu (`NEW GAME` highlighted) → `A` → player name notice → `A` → `Input your player name`.

## Open

- Full in-game campaign / gameplay testing past player-name entry (`swkbd`) and iOS device testing remain to be done.
- Initial boot loading (~0.9 s during shader/asset setup) incurs ~185 audio underruns in the first 2,000 callbacks before settling to 0 underruns in steady state.
