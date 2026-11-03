# Hyrule Warriors: Age of Imprisonment (`04003F40232DE000`)

A Switch 2 game: update v196608, SDK 20.5.15 built for the Switch 2 (`Ounce`). Koei Tecmo's KTGL engine on NVN2, modules `main`, `subsdk0` (NintendoSDK_movie: VP9 movies) and `sdk`. **Plays on the Mac** from a new save through the first-run settings, the opening movies and cutscenes into the first field, where Zelda walks; the frame time's median is 16.6 ms there, with long frames every few frames on a 16 GB Mac ([Performance](#performance)). Battles have not been reached. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x270000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Donkey Kong Bananza's 9 GB layout, as Pokopia's; not measured here. The NPDM declares no system resource |
| `pad_layout full` | `game.tsv` | the camera is on the right stick |

The rewritten modules take 62.5 MB, so the default `mem_used` (64 MB) covers them. The rewriter needs no cut for this build.

## Files

- One [NUP container](../../../docs/development.md#the-packing-script-in-detail) of 47 GB: the base game and update v196608, whose RomFS (46 GB, 2,174 files) is whole; [`pack_nup.py`](../../../scripts/pack_nup.py) packs it without the base's. It carries no NACP or icon; its name record says "HWAoI", so the app takes the profile's name.
- RomFS: KTGL's RDB under `b6e28c40` (`root.rdb`, `system.rdb`, 1,922 `.fdata` files, `data/*.file` and per-language `data/*.file`), and 129 `.bin` archives under `ebc8c781`.

## Shaders

- 101,860 in [`shader_map.tsv`](shader_map.tsv) (101,644 from the RDB, 161 GLSLC outputs in `main`, 55 others), split in about 90 s ([KTGL on NVN2](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata)). The runs here used a partial Mac library of the 15,344 keys asked from boot to the first field ([Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders)): 11,451 of them at boot, as KTGL makes its programs up front. A full Mac build was estimated at 13 hours. [`shader_seen.tsv`](shader_seen.tsv) lists the 15,447 keys of the playtests' library (the first-run settings, the opening, the first field and the areas a 12-minute playtest reached), which [Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders) builds alone: 11,451 of them took about 30 minutes here from an empty cache, the next 3,900 another 29.
- Two of those do not translate (open): `cc679c7b4d3e703c` (vertex, as on Pokopia) and `059a0a910ff87a26` (pixel) load a constant from a bank their address register selects (LDC address mode 2, `.IS`). The translator knows no bank such a load can reach, so even its Maxwell path reads zero there; a fix would declare and bind every bank (up to 18). Neither has been seen drawing.
- Resolved: pixel shaders `02e49c189b769074` and `49241cf469c76830` stopped at the barrier moves around their warp shuffles ([Barrier moves around warp shuffles](../../../docs/reference/shaders.md#barrier-moves-around-warp-shuffles), chain 95ba69b5403e).
- The opening movie's frame is drawn by pixel shader `3aafef4169955790` from `main`; while it was missing from the library the movie stayed black under its "+ Skip" prompt. The movie needs no `tex_flush_upload`.

## Performance

- The first field, from boot: frame time p50 16.6 ms, p95 about 18 ms, p99 100–114 ms on an M6 Mac with 16 GB. Each frame takes 3–6 ms of GPU time; the long frames wait for Metal's submission queue.
- Resolved: about 10 fps (p50 100 ms) in the field. Each dedicated c1 constant block of the partial library was a Metal buffer of its own, and the residency set's 16,118 objects made every commit slow ([Dedicated c1 constants as slices of a bank](../../../docs/reference/frame-rate.md#dedicated-c1-constants-as-slices-of-a-bank)).
- Current (open): the Metal driver wires and unwires memory for each submission, 2.8 s of every 4 s in a Metal System Trace. The game's own GPU memory pool is a constant 4,655 MB (`main+0x19eb098`), with a second pool of 350 MB, and its GPU allocations total 7 GB; the 16 GB Mac had 9.7 GB wired system-wide and about 160 MB free. With fewer commits per frame (`DM_NVN_CUT=0`) p99 was 52–69 ms. A Mac with more memory has not been tried.

## Saves

- The save is one file, `DATA.BIN` (8 MB). A new save creates it at boot with its defaults (operation 0 of the game's file request routine, `main+0x1325be0`); every boot reads its header and three areas (operations 1, 6, 7 and 8).
- The game writes it at its autosave points (operation 2; four of its nine write requests come from the state machine of the save manager, a sequence the game creates only when it saves, `main+0x15463d4`). The opening and the first field reach none, so until then every boot starts at the first-run settings again. Checked: in a 12-minute playtest the game asked for no write and called no file or SDK function to save.
- The autosave icon in the lower right corner after confirming the first-run settings is not a write: the game makes no request and no SDK call while it shows.
- A save written at an autosave point and loaded again has not been seen yet.

## Driving it on the Mac

Run headless: `DM_WINDOW` set to anything, `0` too, opens a window, and a covered window takes the focus away (`oe focus state → 2`), which slows the display link to about once a second and holds `DM_CTL` screenshots. Shots of the 3840x2160 swap chain hold the 1920x1080 picture in its top-left quarter. From a new save: about 40 s to the settings screen, `A` (Next), `A` (Yes), `A` at the autosave notice, then the movies and cutscenes, `A` every 25 s or so; a tap of `Plus` does not skip. Zelda is in the field about 8 minutes after boot, and `LUp` walks. Presses during fades are lost. The work folder has `boot.sh`, `ctl.sh`, `see.sh`, `shaders.sh` and `to_field.sh`.

## Open

- The long frames ([Performance](#performance)).
- Battles, the picture against a Switch 2, an IPA (a full iOS shader build first).

## Found on this game

- [GameChat at boot](../../../docs/reference/services.md#gamechat-at-boot): boot aborts.
- [Whether the language selection is limited](../../../docs/reference/services.md#whether-the-language-selection-is-limited).
- [The target frame rate (graphics frame length)](../../../docs/reference/services.md#the-target-frame-rate-graphics-frame-length).
- [Dedicated c1 constants as slices of a bank](../../../docs/reference/frame-rate.md#dedicated-c1-constants-as-slices-of-a-bank): about 10 fps in the first field.
- [Barrier moves around warp shuffles](../../../docs/reference/shaders.md#barrier-moves-around-warp-shuffles): two pixel shaders left out of the build.
