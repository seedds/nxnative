# Fire Emblem Fortune's Weave (`04007EF01F8AE000`)

A Switch 2 game: v131072, SDK 22.2.0 built for the Switch 2 (`Ounce`), NVN2 API 231.40. Intelligent Systems' engine on NintendoWare (G3d, Ui2d, Vfx, Atk), PhysX 5.6, a deferred renderer. Modules `main`, `subsdk2` (movies) and `sdk`; `subsdk0` (DLSS) and `subsdk1` (CUDA) are not loaded. **Plays on the Mac**: the opening movie, the dialogue and the first battle map, whose first turn matches published footage, and its battle scenes in the user's playtest. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Splatoon Raiders' split as a starting point; the Switch 2's 9 GB |
| `sys_resource 0x4000000` | `game.tsv` | the NPDM declares 64 MB, so virtual address memory is on |
| `pad_layout full` | `game.tsv` | the game asks for most of the controller at once |
| `zip_store .zst:.mp4:.webm:.metallib` | `game.tsv` | the RomFS's files are compressed already |

No `render_cond`: not tried, and nothing has called for it so far.

## Files

- A dump folder holding only `exefs/` and `romfs/`, with no `base/` layout: a RomFS of 28 GB. The NSOs are ZBIC-compressed, so tools that read them need the `zbic` package in the work folder's venv.
- Shaders: 16,888 in [`shader_map.tsv`](shader_map.tsv), from `Shaders/*.bfsha.zst` and the effect and layout packages.
- Movies: 82, AV1 in WebM at 2560x1440 ([AV1 movies through SDK 22's decoder](../../../docs/reference/movies.md#av1-movies-through-sdk-22s-decoder)). A Mac without a hardware AV1 decoder (before M3) decodes them with dav1d.

## Shaders

All 16,888 translate (current). One shader has 10 texture handles the translator cannot resolve (`Failed to find handle source for bindless access of type "texture2D"`) and one logs `Invalid reduction type: F32FtzRn`; neither has been traced to anything visible (open).

## Rendering

- The battle map renders at 1024x576 and DLSS, on MetalFX here, takes it to 2048x1152 ([NVN2's DLSS on MetalFX](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx)), which is scaled to the 3840x2160 window. The network's size follows the display ([The display's largest buffer](../../../docs/reference/services.md#the-displays-largest-buffer)).
- Auto exposure does not meter the scene. `5372c7e7cb7bbda2` writes one luminance into a 16x16 RGBA16F target (134.4 on the first map, log2 7.07), `6d0f6db3f0d396a3` makes a 256-bin histogram of it (the single bin 218), and `a0c632d8b5fa925b` averages its 10th to 90th percentile with a 64-bit division, takes the average through two 256x4 curves and moves a 3x1 R16F image towards the result. The tonemap reads `2^-(image[0] + 3) × 1.44`. On the first map `image` holds 8.96, 18.89, 9.46 with neither of the translator fixes below, and 5.555, 15.195, 6.055 with them, when the picture matches the footage. At a scene change the game clears the image to 1 and dispatches these shaders ([Pending clears land before a compute dispatch](../../../docs/reference/rendering.md#pending-clears-land-before-a-compute-dispatch)).
- Ambient occlusion is interleaved: depth into a 4-layer 512x288 array, four triangles picking the layer by primitive ID, then back together at 1024x576 ([`[[primitive_id]]` in a specialized Metal 4 pipeline](../../../docs/reference/rendering.md#primitive_id-in-a-specialized-metal-4-pipeline)).
- The enemy's checkered look and the pale squares over the ground are in the published footage too: both are the game's.
- Frame time: 33 ms with the window on screen, and GPU about 12 ms. A covered window reads 80-180 ms, the host's display-link fallback (`front end: window covered`), which measures nothing.

## Driving it on the Mac

Keyboard in a window: J K U I are A B X Y, WASD the left stick, Shift + arrows the right stick, Z and C are ZL (threat range) and ZR (settings). The work folder has `win.sh` (a window with Metal's HUD, `run/ctl.txt` as `DM_CTL`), `mash.sh <secs> [dir]` (A every 0.8 s, with shots), `rebuild.sh` (a host copy, `build/fewhost`), `shaders.sh`, and `scripts/shdump.py <key> [ops]` for a shader's raw instructions. Pressing A from boot goes through the movie and the dialogue to the first battle map in about 80 s. Serials shift between runs, so read them from a pass dump; `DM_NVN_PASSDUMP_ALL=1` lists every dispatch's bindings.

## Open

- Nothing past the first map has been played on this build.
- `stage 1 cb4 has no uniform buffer bound`, once a run, with no effect seen.
- MetalFX's jitter sign and DLSS's bias mask are unchecked; the bias mask is only logged. No ghosting has been seen so far.
- An IPA: the 16 GB alias region is over iOS's limit, as for [Splatoon Raiders](../0400380020056000/README.md).
- `dm/games/profile_guess.py` does not recognise the SDK's `Generic` version name.

## Found on this game

- [Scaled shared memory addresses](../../../docs/reference/shaders.md#scaled-shared-memory-addresses) and [IMAD.HI adds a register pair, and IMAD.X its carry](../../../docs/reference/shaders.md#imadhi-adds-a-register-pair-and-imadx-its-carry): the dark map.
- [Pending clears land before a compute dispatch](../../../docs/reference/rendering.md#pending-clears-land-before-a-compute-dispatch): battles and scene changes blown white.
- [NVN2 compute reads its workgroup counts at c0 + 4](../../../docs/reference/shaders.md#nvn2-compute-reads-its-workgroup-counts-at-c0--4).
- [Texture handles of SDK 22's Ampere code](../../../docs/reference/shaders.md#texture-handles-of-sdk-22s-ampere-code): white units.
- [A table of handles past the array's length](../../../docs/reference/shaders.md#a-table-of-handles-past-the-arrays-length): black characters.
- [`[[primitive_id]]` in a specialized Metal 4 pipeline](../../../docs/reference/rendering.md#primitive_id-in-a-specialized-metal-4-pipeline): the dot lattice over the ground.
- [NVN2's DLSS on MetalFX](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx).
- [AV1 movies through SDK 22's decoder](../../../docs/reference/movies.md#av1-movies-through-sdk-22s-decoder).
- [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer) and [The display's largest buffer](../../../docs/reference/services.md#the-displays-largest-buffer).
- [An SDK 21 Switch 2 game with a 4.5 GB alias region](../../../docs/reference/memory.md#an-sdk-21-switch-2-game-with-a-45-gb-alias-region): SDK 22's node area.
