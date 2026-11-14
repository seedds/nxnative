# FAST Fusion (`040041E0212AE000`)

Plays on the Mac at its own 60 fps with [PR #1479](https://github.com/seedds/nxnative/pull/1479): checked on main `6d9edb079` with #1479 (`494a9e568`) and this profile, from boot through the menus into a race (M4 Pro, macOS 27.2, docked); two earlier sessions of about 9 minutes each, on an older tree with the same fixes, ran menus and races and ended by quitting. It needs [PR #1479](https://github.com/seedds/nxnative/pull/1479), which reads its shaders and fixes its rendering: main's shader split does not read its `SHN!D` files.

## What it needs

- [PR #1479](https://github.com/seedds/nxnative/pull/1479) (not merged yet):
  - `shader_split.py` reads Shin'en's `SHN!D` shader files;
  - Ampere TLD through a uniform register (`0xf66`) and a task payload's 16-bit halves: without them the tracks' terrain and the coins, energy bar and award ceremony shaders do not translate;
  - DLSS output keeps the input's alpha; rectangle textures are sampled in texels (the tornadoes); DLSS scalers are released only once their submissions complete (a crash after about 9 minutes of racing, when the game makes its fifth network).
- Two fixes it needed are on main already: `nn::account::LoadProfileImage` gives the local user a picture (the game aborted showing the race results), and the Opus decoder's two forms share one key (a crash at the title).
- The rest of the profile is the survey's guess (`dm/games/profile_guess.py`), unchanged after play: the 9 GB pool of the Switch 2 and a heap region of the pool less 64 MB.

## Files

- A dump folder: the base game and update v262144. The RomFS lists 3,381 files (3.62 GB); the marker is `terrain/10_materials/00_snow_normal.bc7u.tex`. The Mac app is 3.9 GB.
- The display version of v262144 is not known here: the profile's `build` row has `?`.

## Shaders

- 2,976 shaders in Shin'en's `SHN!D` files, all of them translated and built on main with PR #1479 (its reader finds 28 more than an earlier reader did; the tables here are its split's).
- `pso_manifest.tsv` is the host's per-device list from the earlier sessions (2,267 rows); all its shader keys are in this shader map. With it, the app prepares its pipelines in about 1.6 s at its first start, and no pipeline was built at a draw in the race.

## Frame rate

- 60 fps: on main with PR #1479, frame time p50 under 17 ms in all 41 windows of 300 frames (p95 16.9 ms in the race); the long frames are at scene changes (up to about 1 s, loading the next scene's textures). In the earlier 9-minute session, p50 16.65 ms in 106 of 107 windows, 147 of 32,100 frames dropped on screen.

## Found on this game

- The fixes are described in PR #1479's reference entries (`docs/reference/shaders.md`: SHN!D files, a task payload's 16-bit halves, NVN2 compute arrays of handles at a computed c0 address; `docs/reference/rendering.md`: DLSS output alpha, rectangle textures, DLSS scalers).

## Open

- On main with PR #1479 only a race's start was played (one track, Guriin Paaku); the longer sessions ran on a tree of 2026-11-03 with the same fixes but #1479's DLSS scaler change, and did not reach the crash it fixes.
- Logged once, effect not seen: `[RED] NVN unimplemented function called: nvnCommandBufferSetProvokingVertexMode`, `nvnDlssParametersSetOverlayFullScreen` and `nvnDlssParametersSetOverlayDebugLayer`, `[RED] stage 0 cb1 has no uniform buffer bound`, and a cube texture view of a one-face texture read as a 2D array.
- Online play, iOS: not tested.
