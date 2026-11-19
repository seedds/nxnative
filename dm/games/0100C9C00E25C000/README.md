# Mario Golf: Super Rush (`0100C9C00E25C000`)

Update v262144 (4.0.0) over the base game. SDK 11.4.0, NVN. Camelot's engine on NintendoWare (G3d, Vfx), with NEX and Pia for online play. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`, plus one NRO that `main` loads with `nn::ro`. **Plays on the Mac** (M6, 2026-11-13): the opening movie, the title, the main menu, Play Golf's menus, character and course select, and Standard Golf on the Rookie Course's first hole (drives, an approach, a bunker shot, the CPU's turns) at 60 fps. Golf Adventure starts with a Mii the host makes in the editor's place, and plays its opening. Reported playable on an M1 Pro in issue #814, with one hole of one course played. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk`, `subsdk0` in the `build` row | [`game.tsv`](game.tsv) | the NRO imports `movie::Extractor` and the rest of the movie API from `subsdk0`, which `main` does not import; without it in the build row the opening movie stayed black ([A module the NROs import from, missing from the build row](../../../docs/reference/services.md#a-module-the-nros-import-from-missing-from-the-build-row)) |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NSP script's guess for a game with a system resource that never sets a heap (Engage's 5 GB block). `nninitStartup` hands `StandardAllocator` an 8 GB address region, past the 4.38 GB alias region, which the host makes fit |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB, so virtual address memory is on |

Nothing else: no rewriter patch, no per-game host code. The profile moved from the provisional one the NSP script wrote; `shader_groups` and `shader_note` are kept as it wrote them, since both are shader inputs.

## Files

- `main` is 2.3 MB of code. The game's code is a 10.7 MB NRO in the RomFS (`.nrr` registers its hash), loaded at the start of `nnMain`.
- The RomFS is `42` (3.8 GB, Camelot's CRA archive) and the NRR. `42` holds 5,101 files, listed only in the NRO, in the archive's 2021 layout: `dm/gfx/tools/cra.py <42> <NRO> <folder>` writes them out, and all pass the game's checksums ([One scrambled archive indexed by the NRO](../../../docs/reference/shaders.md#one-scrambled-archive-indexed-by-the-nro)).
- Movies are VP9 at 1920x1080, which the game feeds the SDK's decoder in CPU mode (output format 4, NV12).

## Shaders

25,270 in [`shader_map.tsv`](shader_map.tsv), all in `42`; all translate. A full build takes 16 minutes on an M6 from an empty cache, 7.5 minutes after a chain change (the NSP script's logs). [`pso_manifest.tsv`](pso_manifest.tsv) (1,310 lines) was recorded over the bring-up's Mac runs: boot, the movie, the title, the menus, the Rookie Course's first hole.

## Rendering

- The game renders at 1920x1080. On the first hole: frame time p50 16.67 ms, p95 about 17 ms; GPU p50 6.5 ms, p95 8.5 ms; 3.9 GB resident and 3.6 GB of GPU allocations (M6, window with the HUD).
- The course shares textures by storage on a large scale: All-Star Summit registers 134,400 textures and shares about 90,000 as it loads ([A storage takeover walked the sharing table for each texture made](../../../docs/reference/rendering.md#a-storage-takeover-walked-the-sharing-table-for-each-texture-made)). Not loaded here.

## Driving it on the Mac

The work folder `0100C9C00E25C000-work` beside the NSPs has `boot.sh <log> [secs] [VAR=value…]` (window and HUD on, `DM_CTL=ctl.txt`; `DM_SAVE=` for a copy of a save, which must sit in the build folder's `run/` so the pipeline archive beside it is found), `ctl.sh "A" "wait 2" "snap NAME"` (shots in `shots/`), `strip.py` (shots side by side) and `save_ref`, a save with the opening seen.

From `save_ref`: the title's attract movie comes at about 25 s; L+R opens the main menu. Then A on Play Golf, A on Standard Golf, A on Button Controls, A on one controller, A on Stroke Play, A on two players (one CPU), A on Mario, A on the Rookie Course, A on the rule details, and Plus skips the controls pages: the first hole about 20 s later. A starts a shot and A again sets its power. On a fresh save the opening movie plays first (about 60 s).

## Open

- **Resolved: tessellated grass** ([Tessellation stages drawn without a Metal tessellator](../../../docs/reference/shaders.md#tessellation-stages-drawn-without-a-metal-tessellator)). The grass program has hull, domain and geometry stages (vertex `b34da03583527fee`, hull `6c095f74c7d35822`, domain `3fa8664b1e261629`, quads `equal`, geometry `87303af8a75b208a`); its draws were skipped (`geometry draw cannot capture this pipeline (…, tessellation 1, …)`), and the fairway and rough showed their textures without the blades. The hull writes its control points into the stage's circular buffer ([A hull shader's control points written into the stage's circular buffer](../../../docs/reference/shaders.md#a-hull-shaders-control-points-written-into-the-stages-circular-buffer)); the domain's triangles reach the geometry shader with their count written on the GPU. **Current:** the blades are drawn on Bonny Greens' first hole (M6, 2026-11-13): about 267 patches and 6,300 triangles a draw, 60% of the patches culled by the hull, none dropped over the budget; frame time p50 16.7 ms as before (GPU p50 5.4–7.1 ms with and without the grass, within the run-to-run spread of a random opponent). The sea and lava (lava `54fb5ad9deb4c944`, sea `f3fbdabbdbed6058`, `fractional_even` clockwise, hull and domain only) were not reached: no tessellated draw of theirs on the first holes of the Rookie Course, Bonny Greens and Spiky Palms.
- **Golf Adventure's driving range not reached.** The mode starts since [A Mii made in the editor's place](../../../docs/reference/services.md#a-mii-made-in-the-editors-place) (issue #2361): "Select a Mii" lists the new Mii, and the opening and the first evening in the village play (M6, 2026-11-13). The driving-range lesson of [Front and back stencil reference values](../../../docs/reference/rendering.md#front-and-back-stencil-reference-values) comes the next day, past this run's time, so that fix has not been checked here.
- **The player's Mii has no head** without the player's own MiiModel archive (`0100000000000802`, [A console's own Mii database and system data](../../../docs/reference/services.md#a-consoles-own-mii-database-and-system-data)); not run with one. With a Mii on screen ("Select a Mii", the save slot), one `[RED] texture ID … not registered` follows `render target in a compressed texture: view format 0x2f`, once a run; cause not looked at.
- **Resolved: hairline grids** ([Lines wider than a pixel](../../../docs/reference/rendering.md#lines-wider-than-a-pixel), PR #2352). The aiming grid over the Rookie Course's first tee (width 4 and 5, vertex `54767cece6eac379`, fragment `c905fdff3da06415`, 480 segments a draw) is drawn 4 pixels wide (M6, 2026-11-13), for about 12 s from the hole's start; then two one-segment wide lines a frame. Frame time p50 16.67 ms with and without (GPU p50 5.7–7.0 ms on both sides, the spread of a random opponent). The putting grid on the same hole's green (a par 3 reached in one shot; same shaders, 480 segments a draw) is drawn as wide and its moving dashes show clearly (M6, 2026-11-13). Drawn since in the game's own pass, with no capture pass of its own: on the green 65–66 passes a frame that load and store everything instead of 67–68, and about 25 MB less loaded and 25 MB less stored a frame; frame time p50 16.67 ms, GPU p50 6.6 ms against 6.2–6.5 (another opponent, and the pipeline prebuild still running).
- `[RED] texture type mismatch: shader f2ea1078fc963a6b_b (stage 1) declares slot 1 as Metal type 2, texture ID … is type 7 (NVN target 2, format 0x29, …); bound as is`, once per run in play: a `ssg_system/primitive` pixel shader that samples slot 1 as 2D with a 2D coordinate, with a 3D texture bound. The game's binding, not a size query ([A size query on a 3D texture gets a 2D slot](../../../docs/reference/shaders.md#a-size-query-on-a-3d-texture-gets-a-2d-slot) is another pattern). No effect seen.
- `linear texture storage is not in a memory pool (1024x1024 format 0x26)`, once in play, no effect seen.
- **A black screen with the movie decoding**, intermittent: the first boot on the save the provisional build had left showed black for 4 minutes (5 draws per frame from about 10 s on, every shot black, an A press changing nothing). Seen again in about a third of 20 boots from `save` driven through the title and Play Golf's menus (M6, 2026-11-13), with main's host (`2a40e528`) as with the tessellation change: from the title's attract movie on (its full-screen quad drawn black while the decoder runs, 2 colour targets a frame, no guest thread stuck) or after a later menu press; once it recovered at the next screen. The game itself goes on in that state: it takes the menu presses and starts the round (the grass draws of a black run, 2026-11-13), so what is black is the picture, not a stuck game. Cause unknown.
- Not played: other courses, Speed Golf, Battle Golf, Target Golf, Golf Adventure past its first evening (above), motion controls, online play (no network on the host).
- An IPA: `StandardAllocator`'s 8 GB address region is over what an iPhone without the extended addressing entitlement can reserve; not tried.

## Found on this game

- [A module the NROs import from, missing from the build row](../../../docs/reference/services.md#a-module-the-nros-import-from-missing-from-the-build-row): a black opening movie with a provisional profile.
- [One scrambled archive indexed by the NRO](../../../docs/reference/shaders.md#one-scrambled-archive-indexed-by-the-nro): the archive's older layout.
- [A storage takeover walked the sharing table for each texture made](../../../docs/reference/rendering.md#a-storage-takeover-walked-the-sharing-table-for-each-texture-made): All-Star Summit never loaded.
- [Front and back stencil reference values](../../../docs/reference/rendering.md#front-and-back-stencil-reference-values): the driving range's ground decals.
- [A recording's end and links are in control memory](../../../docs/reference/nvn-objects.md#a-recordings-end-and-links-are-in-control-memory): no golfers, trees or buildings without the copy of `CopyCommands`.
- [A Mii made in the editor's place](../../../docs/reference/services.md#a-mii-made-in-the-editors-place): Golf Adventure asks for a Mii and went back to its save slots at a cancel.
- [Lines wider than a pixel](../../../docs/reference/rendering.md#lines-wider-than-a-pixel): the aiming and putting grids were hairlines.
- [A hull shader's control points written into the stage's circular buffer](../../../docs/reference/shaders.md#a-hull-shaders-control-points-written-into-the-stages-circular-buffer): the grass hull had no outputs.
- [Tessellation stages drawn without a Metal tessellator](../../../docs/reference/shaders.md#tessellation-stages-drawn-without-a-metal-tessellator): quad domains in `equal` spacing (the grass).
