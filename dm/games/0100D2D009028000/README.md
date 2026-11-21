# INSIDE (`0100D2D009028000`)

Base game v0 (display version 1.0.0), SDK 3.5.2. Unity 5.6.4p2 IL2CPP on NVN; modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The profile and shader tables are for the base game only.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Unity 5.6 Shader type tree | `dm/gfx/tools/unityfs.py` | The player's serialized files have no type trees. Their five built-in `Shader` objects use type hash `a70f7abc6586fb35b3a0641aa81e9375`; `unityfs.py` now reads that Unity 5.6 layout. The layout matches UnityPy's Unity 5.6 TPK tree node for node, and all five objects read to their exact ends. |
| `sys_resource 0x0`, 4 GiB process map, 256 MiB alias region, application pool `0xCD500000` | `game.tsv` | The NPDM declares no system resource; the profile records the memory layout used by the successful runs. |

## Files and shaders

RomFS has 972 files (1.34 GiB), rooted at `Data/`. The shader split finds 2,246 shaders: 1,149 vertex, 1,085 pixel and 12 compute. Sixteen GLSLC outputs are in `main`; the remaining shader inputs are in `Data/`. The shader tables are for the base game only.

## Runs

- Mac host: a 90-second headless run reached `nnMain` and rendered through frame 900. The final 300-frame sample held 30.0 fps with no dropped frames; there were no `[RED]` or `UNIMPL` lines. The timed run ended with `SIGALRM`. This checks boot and rendering, not a full interactive route.
- iPhone 17 (iOS 27.0): the device log covers about 90 seconds and reports 1,801 presents at 29.9 fps. In the final 300-frame sample, two frames were late; resident footprint was 2,377 MiB, thermal state was 0, and audio reported no underruns. First-use pipeline compilation caused visible frame-time spikes. This is a short run, not a long-session or save/reload test.

The profile has only been checked with the base game. Updates, save/reload, longer play and audio have not been characterized.
