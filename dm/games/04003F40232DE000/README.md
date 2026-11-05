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

- 101,860 in [`shader_map.tsv`](shader_map.tsv) (101,644 from the RDB, 161 GLSLC outputs in `main`, 55 others), split in about 90 s ([KTGL on NVN2](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata)). The runs here used a partial Mac library of the 15,344 keys asked from boot to the first field ([Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders)): 11,451 of them at boot, as KTGL makes its programs up front. A full Mac build was estimated at 13 hours. [`shader_seen.tsv`](shader_seen.tsv) lists the 15,449 keys of the playtests' library (the first-run settings, the opening, the first field and the areas a 12-minute playtest reached), which [Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders) builds alone: 11,451 of them took about 30 minutes here from an empty cache, the next 3,900 another 29.
- Resolved: vertex shader `cc679c7b4d3e703c` (Pokopia's too) and pixel shader `059a0a910ff87a26` stopped at an indexed read of c1 with the bank in the address register ([LDC with the bank in its address register](../../../docs/reference/shaders.md#ldc-with-the-bank-in-its-address-register-is), chain 316fb82e2f22). With it every shader of the playtests' list translates.
- Resolved: pixel shaders `02e49c189b769074` and `49241cf469c76830` stopped at the barrier moves around their warp shuffles ([Barrier moves around warp shuffles](../../../docs/reference/shaders.md#barrier-moves-around-warp-shuffles), chain 95ba69b5403e).
- The opening movie's frame is drawn by pixel shader `3aafef4169955790` from `main`; while it was missing from the library the movie stayed black under its "+ Skip" prompt. The movie needs no `tex_flush_upload`.

## Performance

- The first field, from boot: frame time p50 16.6 ms, p95 about 18 ms, p99 100–114 ms on an M6 Mac with 16 GB. Each frame takes 3–6 ms of GPU time; the long frames wait for Metal's submission queue.
- Resolved: about 10 fps (p50 100 ms) in the field. Each dedicated c1 constant block of the partial library was a Metal buffer of its own, and the residency set's 16,118 objects made every commit slow ([Dedicated c1 constants as slices of a bank](../../../docs/reference/frame-rate.md#dedicated-c1-constants-as-slices-of-a-bank)).
- Current (open): the Metal driver wires and unwires memory for each submission, 2.8 s of every 4 s in a Metal System Trace. The game's own GPU memory pool is a constant 4,655 MB (`main+0x19eb098`), with a second pool of 350 MB, and its GPU allocations total 7 GB; the 16 GB Mac had 9.7 GB wired system-wide and about 160 MB free. With fewer commits per frame (`DM_NVN_CUT=0`) p99 was 52–69 ms. A Mac with more memory has not been tried.

## Resolution upscaling

Measured 2026-11-04 in the first field (Zelda walking), `DM_UPSCALE=2` with `DM_NVN_PASSDUMP_ALL=1`, `DM_NVN_PASSDUMP` of three frames and `DM_CTL` `texlist 1`, before IMAGE-flagged textures could be scaled:

- KTGL makes every render target with `IMAGE | COMPRESSIBLE` (flags `0xc`), and the swap chain's three 3840x2160 textures with `DISPLAY | IMAGE | COMPRESSIBLE` (`0xd`), whether or not a shader ever writes them as images; `DM_NVN_TEXINFO=1920x1080` and `texlist` show no screen-sized target without `IMAGE`. So nothing was scaled.
- The 3D is drawn at a dynamic resolution, 1600x900 in this scene: depth (ser 802, format `0x36`), the G-buffer and lighting targets (863, 1337, 1341, 1348/1349, 3208, 1305) and the post-processed scene (3232), half-size 800x450 targets for effects, all written by draws (19 passes on the depth, 12 on 3208). A frame runs 9 dispatches, and only three of them bind written images.
- Written as compute images: two 1920x1080 targets. `b0fe425c26f12012` is an FSR-style upscale: it reads the 1600x900 scene (3232) with 48 `gather`s at normalized coordinates and writes ser 25094; `0ee13624ced0b38e`, a sharpening pass, reads 25094 with integer `read`s and writes 25095; a draw (`97b3b56827e33df2`) copies 25095 into the top-left quarter of the 3840x2160 swap chain. The third written image is a 4x1 texture (exposure). `2118ab534fac88c6` reads the lit scene 3208 in compute as a texture.
- The movies are drawn by `3aafef4169955790` into a 1920x1080 target (ser 44318); they have no 3D to scale.

So the scene targets only carry the flag, while the upscaler's output is written by compute with the game's thread-group counts and image coordinates. The host therefore scales IMAGE-flagged targets too and demotes a scaled texture to the game's size the first time a shader binds it as a written image (`ups_pick` and `image_prepare` in `nvn_mtl.m`); the swap chain stays at its size, as a target whose scaled size would pass 3840x2160 is not scaled.

Result at 2x (same day, first field): the 1600x900 depth, G-buffer (its two 4-layer 2D arrays included), lighting and scene targets are made at 3200x1800 and stay scaled (24 made, 3 demoted), the game's FSR-style upscale gathers from the 3200x1800 scene, and the field is clean (0 red lines). The three demotions: the upscaler's compute-written outputs 25094 / 25095 ("a shader writes it as an image") and the 1920x1080 depth ser 6, which the pass that copies into the 3840x2160 swap chain clears. The first version brought the 3D back to 1600x900 through one chain of mixed-pass demotions (the unscalable swap chain → ser 6 → the scene 3232 → depth 802 → every G-buffer target); two rules broke it. (1) A pass whose depth disagrees with its colours on the factor runs the depth lookahead (`pass_ds_need`) on the Mac too, and a depth no draw of the pass tests is neither attached nor part of the vote: KTGL draws the swap-chain copy and the scene composite with the depth test off and the depth write on, which writes nothing (as in GL; the host's depth-stencil state sets Metal's write only under the test), and the lookahead now counts a write only under the test. (2) Single-level 2D arrays of up to 8 layers scale (`nvn_ups_shape_ok`): the translator scales integer fetches on 2D arrays as on 2D textures, the host's copies run per layer, and a demotion draws every layer down. Cost on the 16 GB Mac: frame time p50 21.3 ms, GPU-bound (NVN submit + present p50 19.7 ms), against 16.8 ms with the chain demoted; 1.5x has not been measured.

The upscaler bypassed (2026-11-04, branch `ups-bypass-game-upscaler`): with the 3D scaled, the game's FSR-style upscale `b0fe425c26f12012` read the 3200x1800 scene with the tap offsets its constants computed for the game's sizes, so the taps landed between texels and spanned twice their radius: at 2x the first field looked softer than at 1x, "pixel blurred". Its constants are FSR 1's EASU constants (con0 = viewport / output, con1 = 1 / texture size), and they also show that the dynamic resolution renders into a rectangle at the origin of the fixed 1600x900 scene (1439x809, 1151x647 and 1007x566 seen in the first field; the first version of the bypass took the whole texture and showed the picture at 72 % of the frame with stale texels around it). The host now writes the 1920x1080 output itself from that rectangle of the scaled scene ([the game's own upscaler resolved by the host](../../../docs/reference/rendering.md#the-games-own-upscaler-resolved-by-the-host): `ups_bypass_encode`, `nvn_ups_fsr_rect`; `DM_UPSCALE_GAME=keep` runs the game's kernel); the sharpening pass `0ee13624ced0b38e` and the swap-chain copy run unchanged. Shots of the first field (Zelda facing the arch, the same dynamic rectangle 1007x566 in both 2x shots): at 2x with the game's kernel the hair outline and the grass are blocky and doubled, with the bypass they are clean and finer than at 1x (Lanczos-2 crispest; box and tent a little softer, box the cheapest). The resolve costs 0.8 ms a frame with Lanczos-2 on the M2 Pro (2014x1132 → 1920x1080, measured by a 50-dispatch command buffer while another game instance shared the GPU; box 0.2 ms, tent 0.5 ms; `DM_UPSCALE_GAME_FILTER`), about what the game's 48-gather kernel costs: over 45-second windows in the first field the frame time was 24-29 ms (GPU-bound, 34-40 fps) with either, the differences within the drift between windows. The game lowers its dynamic resolution to about 1007x566 at 2x (1439x809 for a moment after loading), as the GPU frame is above its budget; at 1.5x the same rectangle is 1511x849, under the output, so the game's kernel runs there until the resolution rises. Both textures are bound as the game's kernel binds them (RGBA8 views of the sRGB scene and output, format 70: the filter runs in the game's own space). The lower-right of the first field's shots stays the swap chain's black quarter as before. Not measured: battles.

## Saves

- The save is one file, `DATA.BIN` (8 MB). A new save creates it at boot with its defaults (operation 0 of the game's file request routine, `main+0x1325be0`); every boot reads its header and three areas (operations 1, 6, 7 and 8).
- The game writes it at its autosave points (operation 2; four of its nine write requests come from the state machine of the save manager, a sequence the game creates only when it saves, `main+0x15463d4`). The opening and the first field reach none, so until then every boot starts at the first-run settings again. Checked: in a 12-minute playtest the game asked for no write and called no file or SDK function to save.
- The autosave icon in the lower right corner after confirming the first-run settings is not a write: the game makes no request and no SDK call while it shows.
- A save written at an autosave point and loaded again has not been seen yet.

## Driving it on the Mac

Run headless: `DM_WINDOW` set to anything, `0` too, opens a window, and a covered window takes the focus away (`oe focus state → 2`), which slows the display link to about once a second and holds `DM_CTL` screenshots. Shots of the 3840x2160 swap chain hold the 1920x1080 picture in its top-left quarter. From a new save: about 40 s to the settings screen, `A` (Next), `A` (Yes), `A` at the autosave notice, then the movies and cutscenes, `A` every 25 s or so; two taps of `Plus` skip a movie (one tap does not), and in the field `Plus` opens the pause menu, which `B` closes. Zelda is in the field about 8 minutes after boot, and `LUp` walks. Presses during fades are lost. The work folder has `boot.sh`, `ctl.sh`, `see.sh`, `shaders.sh` and `to_field.sh`.

## Open

- The long frames ([Performance](#performance)).
- Battles, the picture against a Switch 2, an IPA (a full iOS shader build first).

## Found on this game

- [GameChat at boot](../../../docs/reference/services.md#gamechat-at-boot): boot aborts.
- [No Game language row in an app packed from a NUP or a dump](../../../docs/reference/packing.md#no-game-language-row-in-an-app-packed-from-a-nup-or-a-dump): its NUP has no NACP. With `DM_LANG` it boots in German, Dutch and Simplified Chinese; Russian and Portuguese, which it lacks, show English.
- [Whether the language selection is limited](../../../docs/reference/services.md#whether-the-language-selection-is-limited).
- [The target frame rate (graphics frame length)](../../../docs/reference/services.md#the-target-frame-rate-graphics-frame-length).
- [Dedicated c1 constants as slices of a bank](../../../docs/reference/frame-rate.md#dedicated-c1-constants-as-slices-of-a-bank): about 10 fps in the first field.
- [Barrier moves around warp shuffles](../../../docs/reference/shaders.md#barrier-moves-around-warp-shuffles): two pixel shaders left out of the build.
- [LDC with the bank in its address register (`.IS`)](../../../docs/reference/shaders.md#ldc-with-the-bank-in-its-address-register-is): two shaders left out of the build.
- [The game's own upscaler resolved by the host](../../../docs/reference/rendering.md#the-games-own-upscaler-resolved-by-the-host): a softened picture at 2x, the dynamic resolution's rectangle from FSR's constants.
