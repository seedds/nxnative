# Hyrule Warriors: Age of Imprisonment (`04003F40232DE000`)

A Switch 2 game: update v196608, SDK 20.5.15 built for the Switch 2 (`Ounce`). Koei Tecmo's KTGL engine on NVN2, modules `main`, `subsdk0` (NintendoSDK_movie: VP9 movies) and `sdk`. **Plays on the Mac** from a new save through the first-run settings, the opening movies and cutscenes into the first field, where Zelda walks; the frame time's median is 16.6 ms there, with long frames every few frames on a 16 GB Mac ([Performance](#performance)). The first battle (the village) plays at 1x and at 2x; the first outpost mission froze the GPU once in the Mac app ([GPU hang in a battle](#gpu-hang-in-a-battle-open)). No IPA has been built.

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

## GPU hang in a battle (open)

On 2026-11-04 the Mac app froze in the first outpost mission while a special attack filled the screen, after 18 minutes at a locked 60 fps (host 48b9911e, `DM_UPSCALE=2` scaling nothing yet). The game kept running (its threads poll and wait) while the GPU showed 100 % utilization for minutes and the host's submit thread waited without a timeout for the oldest submission's fence (`mtl_wait_at`, site WS_ALLOC): the picture froze in silence. The evidence is in the work folder's `logs/freeze-20261104-1938/` (host log, a 5 s `sample`, the pipeline ledger) with a README.

- The four pipeline bases compiled at that instant are (`c13c00c6b467d521`, `1bcc1a39c37f54eb`), (`fd6f2ce7f7046571`, `8d7d98c7b654f6b3`), (`41671ebe006f5bd4`, `328b6d7aa0940939`) and (`4a2396e4f9fea11b`, `f07838d26866e16b`). Pixel shaders `1bcc1a39c37f54eb` and `328b6d7aa0940939` (both from `/b6e28c40/0x38f972af.fdata`, IDRK@0x2f394788) hold KTGL's clustered-lighting loops: the cluster index comes from `gl_FragCoord`, the scene depth (slot 3) and the constants `c3[15..18]` (row and slice strides, z mapping); texel `2i` of the list in slot 16 is the cluster's offset into the index list and texel `2i + 1` packs two counts (`>> 16` and `& 0xffff`); each count bounds a do-while loop that reads a light index from the list and the light's data from slots 17 to 21. A count read from the wrong place (a float's bits give 16,000 or more) runs the loop that often per pixel and the GPU never returns. The vertex shaders have no loops; the other two pixel shaders have none. The pipeline of `1bcc1a39c37f54eb` compiled at the freeze differs from the one its draws use in the first battle only in the blend state (src alpha ONE, dst alpha ONE_MINUS_SRC_ALPHA): the same shader in another material pass, as the special's effect would use.
- The list is a texture buffer of 1,012,000 R32 elements (NVN format `0xb`, serial 3013 in a headless run), zero copy over the pool, bound as 16,384-texel rows (`tbuf_as_2d`) and viewed as R32Float since the shader declares float ([Integer buffer textures read through float slots](../../../docs/reference/rendering.md#integer-buffer-textures-read-through-float-slots)). Checked on this Mac (M2 Pro): such a view returns every bit pattern unchanged, denormal and NaN ones included, and a read past the view's edge returns 0, so an index past the list cannot make a count. The slots 17 to 21 hold RGBA32Float buffers of 1,024 and 3,072 elements and R32 ones of 3,072 and 192; no compute dispatch writes the list (the game's CPU does), and all texture IDs the draws bind lie in the first of the game's two 33,024-entry descriptor pools.
- Not reproduced: the first battle (the village, reached with the route under [Driving it on the Mac](#driving-it-on-the-mac)) draws those pixel shaders at 1x and at 2x and never hangs, but its list is empty in every dump taken (count 0 in every cluster), so the loops never ran there; the special attack's gauge never filled in the scripted rounds, and the outpost mission was not reached headless. The texel the loop read at the freeze is therefore still unknown.
- The host now reports such a hang ([A GPU that never finishes: the hang watchdog](../../../docs/reference/rendering.md#a-gpu-that-never-finishes-the-hang-watchdog)): a `[RED] GPU watchdog` line names the pipelines of the submission the GPU is stuck on. To play on past a fragment shader it names, `DM_NVN_SKIP_FS=<key>` in `~/Library/Application Support/nxnative/04003F40232DE000/env.txt` drops its draws (the effect is missing, nothing else). Next: reach the outpost mission with the watchdog host, `DM_NVN_PASSDUMP_ALL=1` and `DM_NVN_CBDUMP` set, `passdump 3` and a `memdump` of the list right at the special (the scratch scripts `fight2.sh` / `lightlist.py` of the 2026-11-04 session do that), and compare the counts with the pixels' cluster indices.

## Saves

- The save is one file, `DATA.BIN` (8 MB). A new save creates it at boot with its defaults (operation 0 of the game's file request routine, `main+0x1325be0`); every boot reads its header and three areas (operations 1, 6, 7 and 8).
- The game writes it at its autosave points (operation 2; four of its nine write requests come from the state machine of the save manager, a sequence the game creates only when it saves, `main+0x15463d4`). The opening and the first field reach none, so until then every boot starts at the first-run settings again. Checked: in a 12-minute playtest the game asked for no write and called no file or SDK function to save.
- The autosave icon in the lower right corner after confirming the first-run settings is not a write: the game makes no request and no SDK call while it shows.
- A save written at an autosave point and loaded again has not been seen yet.

## Driving it on the Mac

Run headless: `DM_WINDOW` set to anything, `0` too, opens a window, and a covered window takes the focus away (`oe focus state → 2`), which slows the display link to about once a second and holds `DM_CTL` screenshots. Shots of the 3840x2160 swap chain hold the 1920x1080 picture in its top-left quarter. From a new save: about 40 s to the settings screen, `A` (Next), `A` (Yes), `A` at the autosave notice, then the movies and cutscenes, `A` every 25 s or so; a tap of `Plus` does not skip. Zelda is in the field about 8 minutes after boot, and `LUp` walks. Presses during fades are lost. The work folder has `boot.sh`, `ctl.sh`, `see.sh`, `shaders.sh` and `to_field.sh`.

## Open

- The long frames ([Performance](#performance)).
- [The GPU hang in a battle](#gpu-hang-in-a-battle-open): the outpost mission, with the special attack's light list dumped.
- The picture against a Switch 2, an IPA (a full iOS shader build first).

## Found on this game

- [GameChat at boot](../../../docs/reference/services.md#gamechat-at-boot): boot aborts.
- [No Game language row in an app packed from a NUP or a dump](../../../docs/reference/packing.md#no-game-language-row-in-an-app-packed-from-a-nup-or-a-dump): its NUP has no NACP. With `DM_LANG` it boots in German, Dutch and Simplified Chinese; Russian and Portuguese, which it lacks, show English.
- [Whether the language selection is limited](../../../docs/reference/services.md#whether-the-language-selection-is-limited).
- [The target frame rate (graphics frame length)](../../../docs/reference/services.md#the-target-frame-rate-graphics-frame-length).
- [Dedicated c1 constants as slices of a bank](../../../docs/reference/frame-rate.md#dedicated-c1-constants-as-slices-of-a-bank): about 10 fps in the first field.
- [Barrier moves around warp shuffles](../../../docs/reference/shaders.md#barrier-moves-around-warp-shuffles): two pixel shaders left out of the build.
- [LDC with the bank in its address register (`.IS`)](../../../docs/reference/shaders.md#ldc-with-the-bank-in-its-address-register-is): two shaders left out of the build.
- [A GPU that never finishes: the hang watchdog](../../../docs/reference/rendering.md#a-gpu-that-never-finishes-the-hang-watchdog): the freeze in the first outpost mission ([GPU hang in a battle](#gpu-hang-in-a-battle-open)).
