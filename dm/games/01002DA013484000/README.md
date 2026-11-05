# The Legend of Zelda: Skyward Sword HD (`01002DA013484000`)

Update 1.0.1 (v65536) over the base game, SDK 11.4.0. Nintendo's Wii-era engine, ported by Tantalus, on NVN. Modules `main`, `subsdk0` (GLSLC, NVN's runtime shader compiler), `subsdk1` (NintendoSDK_movie) and `sdk`. Tested on the Mac only, from a new game through the opening to Skyloft: 60 fps, about 1.8 GB resident, no red lines. Combat, the Sailcloth, flight, the dungeons and the iPhone are not tested here; the reporter of issue #928 played on through the Sailcloth tutorial on 1.0.0 and 1.0.1.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Every pixel shader writing exactly colours 0 and 1 | [`dual_want.tsv`](dual_want.tsv) | see [Rendering](#rendering) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game's last `nvnDeviceSetDepthMode` at boot is near = 0, so the pipeline bases made before it runs take the `main0_z` vertex entries |
| 4 GB layout: `mem_reserve 0xd0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource in the NPDM: the game uses `SetHeapSize`, 696 MB of bss, so a 2.5 GB heap region (it takes 2.46 GB) |

No rewriter cuts of its own, no `patches.py`.

## Rendering

- **Resolved: a brown cast over the distance, near objects as dark silhouettes, in the title's sky and the opening's flight (issues #497, #928).** It came and went with the camera, because it depends on how much fog is on screen. `Dof_FogToAlpha` (`f11c15a99b0c1790`) writes `(0, 0, 0, fog)` with logic operation OR into the frame's RGBA8Unorm target, whose alpha was just cleared to 0, to hand the depth of field its fog in the alpha. The host drew logic operations only on integer targets, so here it wrote black into the colours. General fix: [Logic operations through a library of its own](../../../docs/reference/rendering.md#logic-operations-through-a-library-of-its-own). `eb1648acad4e38e3` takes the same path in the Knight Academy.
- **Current: dual-source blending, listed whole.** Link's eyes and the Sailcloth, among others, blend output 1 as the second source ([A written secondary blend source](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending)). Issue #928 found six such shaders by playing, each a visible bug until listed (oversized eyes, an invisible Sailcloth), so the list holds all 432 pixel shaders that write exactly colours 0 and 1, the six among them, from a census of the translated shaders. A pipeline takes the entry point only when it blends with source 1, so the rest cost one unused entry point each. None of the passes dumped on the title, in the Academy hall and outdoors in Skyloft has a second colour target (59 passes).
- **Resolved: the shaders.** `Shaders/Shaders.arc.gz` holds 2,412 of the 2,470: [Gzip-wrapped shader archives](../../../docs/reference/shaders.md#gzip-wrapped-shader-archives). The other 58 are the loose `Shaders/custom` files. `Shaders/Dummy.vert` and `Dummy.frag` are encrypted GLSL that the game compiles only for a program the archive lacks: [Encrypted `.vert` and `.frag` files](../../../docs/reference/shaders.md#encrypted-vert-and-frag-files-are-no-run-time-glsl).

## Open

- **A torn movie picture (issue #497, not reproduced).** That report saw a horizontal band near the top of the opening movie, rows from two successive pictures, and proposes keeping a snapshot of the texture storage for each upload. On an Apple M6 here, 114 shots of `THP/Demo01_01.webm` show no row boundary that jumps across the width. Whether the race can happen here is not settled.
- **Dash speed from a tilted stick (issue #928, not reproduced).** That report has Link running at dash speed without B at some diagonal angles, because a host stick can go outside the circle a Switch stick stays in, and proposes scaling every stick back onto the circle in `hle_hid.c`. In the Academy hall, a full diagonal for 1.2 s and the opposite diagonal on the circle for 1.2 s brought Link back to the same spot, so the game showed no extra speed at that angle. A shallower angle was inconclusive, the camera having turned. The change would reach every game, so it waits for a measurement.

## Mac routes

The work folder is `<NSP folder>/01002DA013484000-work`, with `boot.sh`, `ctl.sh`, `go.sh`, `mash.sh`, `health.sh` and the saves.

- A fresh save needs `DM_KBD=Link` for headless runs: the name entry is the software keyboard, and without an answer it counts as cancelled and the file list comes back.
- `A` or `Plus` skips the opening movie. At the title, `A~0.4`. Wait for the file list to settle (about 5 s; a press during its fade does nothing), then `LDown~0.25` and `A~0.3`: the stick moves the highlight, and it fades when the stick lets go, so send them one right after the other.
- From there `A` skips through the cutscenes to Link's room, about 4 minutes. The academy's way out is the carved double door behind the round carpet in the hall. The autosave reaches `save_ref` in Skyloft, outside that door.

## Found on this game

General host changes first needed here; each applies to every game:

- **The shader libraries as one file, read in place** (`shader_lib.py`, `msl_image.h`): [Shader libraries take a file per shader variant](../../../docs/reference/packing.md#shader-libraries-take-a-file-per-shader-variant). Measured here: the route from `save_ref` to the file list, 6,600 frames from the folder and from the image, with no `[RED]` line in either and pictures alike.
- **Logic operations on normalized targets** (`dmsh_logic.py` `dm_logic_norm`, `nvn_mtl.m` `lop_key`, `unorm_chan_masks`): [Logic operations through a library of its own](../../../docs/reference/rendering.md#logic-operations-through-a-library-of-its-own).
- **Plain gzip around a shader archive** (`shader_split.py` `ungzip`): [Gzip-wrapped shader archives](../../../docs/reference/shaders.md#gzip-wrapped-shader-archives).
- **`.vert` and `.frag` files count as run-time GLSL only as text** (`profile_guess.py` `glsl_text`): [Encrypted `.vert` and `.frag` files](../../../docs/reference/shaders.md#encrypted-vert-and-frag-files-are-no-run-time-glsl).
- **A shader's cache key covers its `dual_want.tsv` row** (`dmsh_build.py` `shader_key`): [A shader added to `dual_want.tsv` keeps its cached build](../../../docs/reference/rendering.md#a-shader-added-to-dual_wanttsv-keeps-its-cached-build).
