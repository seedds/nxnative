# Resident Evil 0 (`010097000BC10000`)

Switch base version 1.0.0 (`v0`), SDK 5.4.0. Capcom's MT Framework on NVN with `NintendoSDK_movie` (`subsdk0`). Modules `main`, `subsdk0`, `sdk`.

Build ID of 1.0.0's `main` (first 16 hex digits): `FB4239AA962B429B`.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | NPDM declares 0 MB of system resource, so `main` sets a 3.3 GB heap |
| `present_interval display`, `fps_target 120`, `fps_max 120` | [`game.tsv`](game.tsv), `dm/host/hle_fpslock.c` | Writes only the MT Framework render frame limit (`sMain::mpInstance + 0x4c`), leaving `+0x48` at the game's simulation-time scale |
| `movie_pacing on` | [`game.tsv`](game.tsv), `dm/gfx/nvn/hle_nvn.c`, `dm/host/hle_fpslock.c` | Matches render pacing and presentation interval (`30` or `60` Hz, respecting thermal capping) to the active decoded movie rate (`dm_movie_hz()`) across 30/60/120 fps profiles |
| `screen_match off` | [`game.tsv`](game.tsv), `dm/host/front_mac.m` | Keeps the display's refresh rate in fullscreen unless overridden by `DM_SCREEN_MATCH` |
| ProMotion 120 Hz display link floor on iOS | `dm/gfx/nvn/nvn_mtl.m` | Uses `CAFrameRateRangeMake(80, 120, 120)` in mode 2 on iPhone/iPad so ProMotion does not settle on 60 Hz |
| On-demand shader table derivation | `scripts/build_ipa_from_nsp.py` | Pre-split shader tables are omitted from the profile and derived into a shader-only build profile when splitting the user's own game files |

## Shaders

This profile ships empty placeholder shader tables (`EMPTY_TABLES`). During `scripts/build_ipa_from_nsp.py`, a registered NVN profile with no pre-split `shader_map.tsv` data rows derives a shader-only build folder and keeps all `game.tsv` keys and host fixes intact.

## Frame rate

- `hle_fpslock.c` targets `main` build `fb4239aa962b429b3d1776af92d93e79` at `0x1baac60` (`+0x4c`, `FPS_F32`, 1 copy).
- While a movie plays (`dm_movie_hz() != 0`), `hle_nvn.c` (`high_cap_tick`, `nvn_fps_high_now`, `present_itv`) and `hle_fpslock.c` clamp both presentation intervals and the MT Framework render limit to the movie's picture rate (`30` or `60` Hz, or `30` Hz under thermal cap) so audio and video remain synchronized under 30, 60, and 120 fps targets.

## Open

- Shader prewarm manifest (`pso_manifest.tsv`) and `validated.tsv` baseline runs are not yet recorded.
