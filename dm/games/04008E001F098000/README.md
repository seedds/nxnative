# Nintendo Switch Sports Resort (`04008E001F098000`)

A Switch 2 game: update v65536, which holds the whole game (packed from a NUP with no base game and no control data). Nintendo EPD's sead / agl / gsys with Havok, SDK 22.2.10 built for the Switch 2 (Ounce), NVN2. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`; `subsdk1` to `subsdk4` are not loaded.

Tested on an Apple-silicon Mac (M2 Max, 32 GB), macOS 27.2, as a Mac app with its full offline pipeline archive (710.7 MB): it boots, goes through the menus, the motion calibration and the sports, and plays matches with motion controls from a Joy-Con 2 (R, over Bluetooth LE with `DM_SW2=1`) or a DualSense, or with the keyboard and its scripted swings. A 37-minute session on 2026-11-11 played without an error, with no compute pipeline built at run time and a frame time p50 of 33.3 ms (the game presented at 30 fps there). Not run on an iPhone or iPad. It plays only with the host changes listed under [Depends on](#depends-on).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x2000000` | [`game.tsv`](game.tsv) | the NPDM's 32 MB of system resource: virtual address memory |
| `mem_reserve 0x318000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | the Switch 2's 9 GB pool, and the 7.47 GB heap the game asks for (the NSP script's heap check). In its first 15 s the game's regions reach alias +1.09 GB; in play +3.26 GB was seen |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0, so the pipeline bases made before it runs take that mode |
| `render_cond on` | `game.tsv` | the game wraps clears in render conditions; with the key about 10 redundant clears a frame are skipped, with no visible change. See [Rendering](#rendering) |
| `pso_manifest.tsv`: 31 `M` rows | [`pso_manifest.tsv`](pso_manifest.tsv) | the native task / mesh pipelines, which the offline pipeline archive does not hold. See [Pipelines](#pipelines) |
| `DM_ROMFS_OVERLAY` with patched resource size tables | the player's `env.txt` (outside the repository) | a stopgap, still used in the sessions above: the game's model heaps are sized from its resource size table, and the host's command records overrun them ([Model heaps sized by the host's command record sizes](../../../docs/reference/memory.md#model-heaps-sized-by-the-hosts-command-record-sizes); open, see [Open](#open)) |

There is no game-specific patch and no rewriter cut.

## Depends on

The game runs on main only with these host and tool changes, each its own pull request (branch names of the fork, `bymaxmarc/nxnative`):

- Boot and saves: `pr/fs-sdk-mounts` (file calls on its VoiceText RAM disk, which only the SDK holds), `pr/fs-save-readonly` (the read-only mount of its own save; without it every start after the first save aborts at 48 s), `pr/audio-virtual-output` (the virtual output device's properties and its OutputStream).
- Controllers and motion: `pr/hid-sw2-services-styles` (a whole controller shown as a Joy-Con pair, as the game supports Handheld | JoyDual only: without it the controller applet opens every frame; the Joy-Con icon decoration colour), `pr/hid-imu-samples` (the Switch 2 hid IMU calls its sensor fusion reads, the sample layout, live motion from real controllers), `pr/hid-scripted-swing` (`DM_KEYS` / `DM_CTL` `Swing` and `Lift`, V and B on the Mac keyboard).
- Pictures: `pr/sr84-fragment-size` (the main menu's sea and the avatars were black), `pr/rgb9e5-r32ui-views` (a uint draw into an R32_UI view of an RGB9E5 target), `pr/uniform-tests-volatile` (shaders whose uniform tests Apple's compiler folded into a switch).
- Pipelines and frame time: `pr/pso-archive-split` (the offline pipeline archive builds whole although a few groups fail in `metal-tt`), `pr/cps-async` (compute pipelines that take Apple's compiler minutes no longer stop the replay), `pr/staged-upload-src` and `pr/residency-commit` (the scene loads, below).
- Diagnostics used here: `pr/nvn-cmdstat` (`DM_NVN_CMDSTAT`, for the model heaps).

## Pipelines

- Build the Mac app with its pipeline archive (the NSP script's default; `--gpu none` leaves it out): 710.7 MB on the M2 Max with the split archive build above (an earlier build without it held 10,784 bases and 291 compute pipelines in 590 MB). Without an archive every new draw waited for its pulling base: in one 13-minute session with no archive and `DM_PSO_PREBUILD=0`, 247 new pipelines stalled frames for 23.3 s in all, the worst 5.7 s at the start of a match (`draws that waited: pulling base not made yet 110 / 5973 ms`). With the archive, on the same device lists, no long frame had a new pipeline.
- `pso_manifest.tsv` lists only task / mesh pipelines (31): the offline archive holds none. 22 are what the device's own list recorded over boot, the menus and several sports; on a first start (no `pso_user.tsv`, no device archive, the same 110 s route) the list prepares in 0.01 s, and the 18 the route meets are no longer built at their first draw (0.1-1.1 ms each with the Metal shader cache warm; a cold cache is not measured). The device's whole list (5,698 rows) as the seed instead spent 19.4 s preparing before the first frame, compiling 1,376 bases with a vertex layout the offline archive does not hold; without it the first 110 s of play built about 200-220 render pipelines, about 0.3 s spread over the long frames, while hybrid pipelines drew. The 37-minute session of 2026-11-11 still built 9 task / mesh pipelines the list lacked at their first draw, all but two with task shader `ffb46e1e142722c3_b`: 65-800 ms each, 1.7 s in one frame of a sport's load (frame 51,749); the list holds them too now.
- A few compute shaders take Apple's compiler minutes; they come from the archive (and `pr/cps-async` keeps a missing one from stopping the replay).
- Six mesh shaders of the sea (agl `AnnaSea`) are refused when built: their task payloads are over Metal's 16 KB. No draw has been seen to need them in the sessions so far.

## Frame rate

- **60 fps, present interval 1, in the tennis match** (M2 Max, window, 240 Hz screen): frame time p50 16.67 / p95 17.2 ms, NVN submit about 6.8 ms and GPU about 12 ms a frame, CPU about 145% of one core, no frame dropped on screen. The game sets interval 2 for a few frames around scene loads; the 37-minute session above presented at 30 fps (p50 33.3 ms).
- **Scene loads.** The first frame of a scene draws every texture the scene loaded: 855 textures (908 MB) for the court behind the sport menu, about 225 ms, nearly all of it on the submit thread, about 150 ms of it committing the residency set: the new textures, their staging and their source buffers, about 0.04 ms per MB while the GPU runs the previous frames (`pr/staged-upload-src` and `pr/residency-commit` brought it there). Smaller loads: 92 textures about 110 ms, 340 about 135 ms. Making the textures when the game registers them removed the court frame's hitch, but the game registers about 13,800 packaged textures at boot that it never draws (GPU memory 3.0 → 6.5 GB), so the host does not do that.
- **Boot.** One frame about 7 s in runs 250 ms on the GPU: the game draws 1,657 render passes in it, 400 of them one-draw 256x32 RGBA16F passes of up to 1.8 ms each. The next frame waits for it.
- **When measuring:** a run's screenshots (`DM_NVN_SHOT`) each cost a frame of 120-190 ms. On a 75 Hz screen (seen in unattended runs) 60 fps shows as one frame of 26.7 ms in every four, by the screen's rate, not the game. Heavy compiles beside the game (an offline pipeline archive build) took a session from 60 to 22-40 fps: the game's main thread spins on its workers, which then lack CPU.

## Rendering

- `render_cond on`: about 10 clears a frame are conditional on render-enable state the game sets; with the key they are skipped as on the console. No visible change in the menus and the match.
- The main menu's island sea drew black with NaN noise, and the avatar's hair and clothes drew flat black in Personalizar, user selection and the HUD icons, until the fragment size register read as 1 x 1 (`pr/sr84-fragment-size`): the sea shaders `01a6b85e2f6ea3d4`, `b3203175c76cda20` and `2271b2e0293b3c55` divided by it, and the avatar material `c30f0edd6c669a61` took it into a shading factor. On the rebuilt app the sea is blue and two random default avatars show their colours in Personalizar (`DM_CTL` route, runs of 2026-11-10 15:11 and 15:15).

## Motion controls

- The game reads motion through the SDK's own sensor fusion (`hid::UpdateSensorFusion` → `GetNpadImuSamples`), which `pr/hid-imu-samples` serves. It supports Handheld | JoyDual only, so the keyboard, a DualSense or a single Joy-Con is shown to it as a Joy-Con pair (`pr/hid-sw2-services-styles`).
- Checked by the user on the M2 Max: the motion calibration ("place the Joy-Con on a flat surface") passes, and matches play with the paddle and racket following a right Joy-Con 2 or a DualSense. Before the IMU sample layout fix, the calibration failed after 30 s every time and the paddle spun by itself.
- Without a motion controller: V and B on the Mac keyboard swing player 1's controller once (forehand, backhand), and `DM_KEYS` / `DM_CTL` take `Swing` and `Lift` steps.

## Memory

- In the tennis match: resident (footprint) about 9.7 GB, GPU allocated 5.6-8 GB.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NUP or dump folder> --platform mac` and use its printed command.
- From an existing save: `DM_KEYS="20:A*60/3"` (A every 3 s from 20 s) goes through the menus into a doubles tennis match, at its serve prompt by about frame 5,000.

## Open

- The model heaps the host's command records overrun, and the `DM_ROMFS_OVERLAY` stopgap with patched resource size tables: five or six NPC models (`Npc_IntegrationYoung*`) load at once, each into its own 4 MB heap sized from the resource size table (`System/Resource/ResourceSizeTable.Product.110.Nin_Ounce_NVN2.rsizetable.zs`), which runs out at about frame 130 without the overlay.
- The system data the game mounts as `MiiResource` (`0400000000000849`) is not present on the host: `[RED] fs mount system data MiiResource: the host has no 0400000000000849`; nothing visibly missing was noticed.
- Six `AnnaSea` mesh shaders refused (task payloads over 16 KB), above.
- The court's first frame (about 225 ms) and the other scene loads; the boot frame.
- Two `[RED] texture type mismatch` lines in the 37-minute session (shaders `7309860342dd3b14_b` and `34e7cedf5d2acf7c_b` declare a 2D texture where a 2D array is bound), once each; their effect is not checked.
- Sports other than tennis are not checked in a scripted route.
- iPhone and iPad: no IPA built.

## Found on this game

- [Home button protection on a Switch 2 controller](../../../docs/reference/services.md#home-button-protection-on-a-switch-2-controller) (the question it asks at boot)
- The reference entries of the pull requests under [Depends on](#depends-on) name this game; they link this note once both are on main.
