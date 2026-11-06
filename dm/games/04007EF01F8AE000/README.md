# Fire Emblem Fortune's Weave (`04007EF01F8AE000`)

A Switch 2 game: v131072, SDK 22.2.0 built for the Switch 2 (`Ounce`), NVN2 API 231.40. Intelligent Systems' engine on NintendoWare (G3d, Ui2d, Vfx, Atk), PhysX 5.6, a deferred renderer. Modules `main`, `subsdk2` (movies) and `sdk`; `subsdk0` (DLSS) and `subsdk1` (CUDA) are not loaded. **Plays on the Mac**: the opening movie, the dialogue and the first battle map, whose first turn matches published footage, and its battle scenes in the user's playtest. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Splatoon Raiders' split as a starting point; the Switch 2's 9 GB |
| `sys_resource 0x4000000` | `game.tsv` | the NPDM declares 64 MB, so virtual address memory is on |
| `pad_layout full` | `game.tsv` | the game asks for most of the controller at once |
| `zip_store .zst:.mp4:.webm:.metallib` | `game.tsv` | the RomFS's files are compressed already |
| `fps_target 60` | `game.tsv` | the host writes 60 into the game's target frame rate, which the rewriter finds by its readers in any build; see [Frame rate](#frame-rate) |
| `fps_max 120` | `game.tsv` | **Frame rate** offers 120 on the Mac: speed measured below; see [Frame rate](#frame-rate) |
| `depth_mode zero` | `game.tsv` | the game selects near-zero NVN depth; offline pipeline bases must use the matching vertex entry. Without it the app's bundled archive (661.6 MB on an M1 Pro) was refused at every start and the Mac built 872 MB of bases of its own in the background |

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
- Frame time: 33 ms with the window on screen at the game's own 30 fps (16.7 ms at `fps_target 60`, [Frame rate](#frame-rate)), and GPU about 12 ms. A covered window reads 80-180 ms, the host's display-link fallback (`front end: window covered`), which measures nothing.

## Frame rate

- **Current:** 60 fps (30 in gameplay as shipped). The game's graphics object, a global at `[main+0x21e60d8]`, holds the display rate at +0x1d8 (60), the target frame rate at +0x1dc and the longest present interval at +0x1e0 (5). The present (`main+0x648da0`, through `gxc::NvnDevice`'s present, its vtable's slot 3 at `main+0x5d170`) asks for interval min(+0x1e0, +0x1d8 / min(+0x1d8, +0x1dc)).
- The game sets the target to 60 at `main+0x652960` and `main+0x66af3c` (the title and menus run at 60) and to 30 at `main+0x7bf1c0`, where gameplay starts, which makes it interval 2. 59 readers step the game by 60.0 / target (`main+0x770d5c`, `main+0x11e6310`, `main+0x13a6c94`): 2.0 a frame at 30, 1.0 at 60.
- The rewriter finds the field by those readers and the store of 30, with no build ID ([A target frame rate the game steps by](../../../docs/reference/frame-rate.md#a-target-frame-rate-the-game-steps-by); `dm/tools/fps_target.py`: 55 of the 59 readers have the pattern it matches, `FPS_TARGET_PTR 0x21e60d8`, `FPS_TARGET_FIELD 0x1dc` in main's fixups), and the host writes the profile's `fps_target` there on every present where it differs (`hle_fpslock.c`, "target frame rate"). The game then asks for interval 1 itself. On an M4 Pro the battle map holds frame time p50 16.7 ms, p95 17.1 ms, about 2 frames of 300 dropped, in a window. Headless with the found field (2026-11-03, M4 Pro, A every 0.8 s from the title into gameplay): the game sets interval 1 → 2, the host writes 30 → 60, the game sets interval 2 → 1 (`main+0x5d1c8`), p50 16.6-16.7 ms through frame 7200.
- `DM_FPS_TARGET=30` (the settings window's Frame rate) writes 30, menus included, where the game itself chose 60.
- Not measured: game speed at 60 against 30 over the same route (the player saw battles at 60 with nothing reported fast).
- **Current: 120 fps on the Mac when chosen** ([120 fps follows the frame step](../../../docs/reference/frame-rate.md#120-fps-follows-the-frame-step)). The present (`main+0x649920`) still asks for interval 1, as 60 / min(60, 120) = 1, and all 60 reads of the target found through the global are floats, so the step takes 0.5. Nothing ties this to a build: the rewriter finds the field in any main, and `fps_max` is not gated. v131072 is the only version tested. The target follows what fits, so frames at 60 step by 1.0 and the game keeps its speed; before that, the same field at a fixed 120 would have run a 70 fps scene at 58%.
  - Speed: from one save on a battle map, the same timed cursor holds (0.3, 0.5 and 1 s, then back) at 120 and 60 stop at the same place on all eight shots: the best match between each pair is a shift of zero pixels (2026-11-04, M6, headless). The user played at 120 and saw the speed as right. The holds are in whole tiles and the rest was judged by eye, so a difference of a few percent would not show.
  - Frame rate: on the map, headless, 107-119 fps with 96-100% of frames at 120 per 1,800 frames (frame time p50 8.3 ms, GPU 5.5 ms, about 840 draws). In the window on the prototype, without the fallback, most of the user's session held 106-117 fps at 600-990 draws, and 65-72 fps where the GPU took 12-13 ms; that stretch would now run at 60. Added lag of 12 ms a frame holds a steady 60 with the target at 60. In a window the host picks the 120 Hz display link by itself (open: the frame rate of this build in a window; the one short run had two other games running on the Mac and measured nothing).
  - Known: `game::telop::ListResultMenuItemBase` (`main+0x17885f8`) adds max(int(step), 1) to a timer each update, 8 times that while an input flag is set, so its In / Out timer runs at double speed at 120. It is a results list's menu item, a matter of looks, and is left as it is (a cut would tie this to the build). Systems that ignore the step altogether would also run fast; none has been seen so far.

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
- [Bindless shaders taken as depth readers](../../../docs/reference/rendering.md#bindless-shaders-taken-as-depth-readers): the battle maps' decals split the G-buffer pass about 31 times a frame.
- [System applets opened by `Show…` calls](../../../docs/reference/services.md#system-applets-opened-by-show-calls): a null read in `nn::applet::CreateLibraryApplet` at the title about 100 s in, the strap guide overload with a `StrapGuideArg`.
- [A target frame rate the game steps by](../../../docs/reference/frame-rate.md#a-target-frame-rate-the-game-steps-by): gameplay at 60 fps.
- [120 fps follows the frame step](../../../docs/reference/frame-rate.md#120-fps-follows-the-frame-step).
- [AV1 movies through SDK 22's decoder](../../../docs/reference/movies.md#av1-movies-through-sdk-22s-decoder).
- [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer) and [The display's largest buffer](../../../docs/reference/services.md#the-displays-largest-buffer).
- [The output device notification under its SDK 21 and 22 name](../../../docs/reference/audio.md#the-output-device-notification-under-its-sdk-21-and-22-name): an abort at start without it.
- [An SDK 21 Switch 2 game with a 4.5 GB alias region](../../../docs/reference/memory.md#an-sdk-21-switch-2-game-with-a-45-gb-alias-region): SDK 22's node area.
- The title screen's hand-over to the opening demo finalizes a program its queue still has bound: [Finalized programs leave every queue's bindings](../../../docs/reference/nvn-objects.md#finalized-programs-leave-every-queues-bindings) (resolved).
