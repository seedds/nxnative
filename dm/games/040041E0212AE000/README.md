# FAST Fusion (`040041E0212AE000`)

Plays on the Mac at its own 60 fps: boot, the menus and races, in two sessions of about 9 minutes each (M4 Pro, macOS 27.2, docked), each ended by quitting. It needs [PR #1479](https://github.com/seedds/nxnative/pull/1479), which reads its shaders and fixes its rendering: main's shader split does not read its `SHN!D` files.

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

- 2,948 shaders in Shin'en's `SHN!D` files, all of them translated and built (with PR #1479's reader and fixes).
- `pso_manifest.tsv` is the host's per-device list from the sessions played (2,267 rows).

## Frame rate

- 60 fps: frame time p50 16.65 ms in 106 of the 107 windows of 300 frames of the second session (M4 Pro, docked); 147 of its 32,100 frames were dropped on screen.

## Found on this game

- The fixes are described in PR #1479's reference entries (`docs/reference/shaders.md`: SHN!D files, a task payload's 16-bit halves, NVN2 compute arrays of handles at a computed c0 address; `docs/reference/rendering.md`: DLSS output alpha, rectangle textures, DLSS scalers).

## Open

- The sessions here ran on a tree of 2026-11-03 (main `2c35f7b3e` with this game's fixes, those of PR #1479 but its DLSS scaler change), not on main with PR #1479 itself. Neither session reached the crash that change fixes.
- Logged once in the second session, effect not seen: `[RED] NVN unimplemented function called: nvnCommandBufferSetProvokingVertexMode`, `[RED] stage 0 cb1 has no uniform buffer bound`, and a cube texture view of a one-face texture read as a 2D array.
- Online play, iOS: not tested.
