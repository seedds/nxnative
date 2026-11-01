# DOOM (`0100416004C00000`)

Update 1.4 (v262144) over the base game, SDK 6.4.0. id Software's id Tech 6 on NVN, with Wwise audio. Modules `main` and `sdk`; `subsdk0` (1,659 instructions) is not loaded, since `main` takes nothing from it that the host does not supply. RomFS is id Tech 6's `.resources` containers with their `.pindex` indexes, which the game reads itself. Plays on the Mac and on the iPhone: the menus and the campaign's first levels at a steady 30 fps (0 to 5 late frames in 300 on the iPhone, a median of 16–17 ms of GPU time a frame) with sound. The footprint is about 5.1 GB, so the iPhone build needs a provisioning profile with the increased memory limit entitlement.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sync_unfenced signaled` | [`game.tsv`](game.tsv) | the render thread waits on its frame sync before it has fenced it once; with `timeout` the boot stopped at "Initializing renderSystem media" (see [Boot](#boot)) |
| `shader_sb_unaligned on` | `game.tsv` | the depth pre-pass's geometry shader appends 16-bit index lists at 2-byte boundaries (see [Rendering](#rendering)) |
| [`geo_mesh.tsv`](geo_mesh.tsv), 15 pairs | profile | the pre-pass's culling geometry shaders drawn as mesh libraries; without them every geometry-shader draw split the render pass (see [Performance](#performance)) |
| `present_choice on` | `game.tsv` | the game asks for interval 2; id Tech 6 runs at any frame rate on PC, so the launcher offers 60 fps (`display`) and 60 falling back to 30 (`adaptive`). Game speed at 60 looked right but was not measured |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`; `sys_resource 0x0`; `mem_used 0x5b28000` | `game.tsv` | no virtual address memory: `main` sets a heap of at most the pool − 64 MB − its 42 MB of bss. The survey's values, unchanged |
| `shader_groups /base/ /virtualtextures/` | `game.tsv` | the shader split's groups, by RomFS top-level folder |

## Boot

- **Resolved: the boot hung at "Initializing renderSystem media".** The render thread polls its frame sync with `nvnSyncWait` before it has put a fence on it, and the host answered "timeout" for a sync that was never fenced, so the thread waited forever. A Switch answers that wait at once. The profile key `sync_unfenced signaled` (`hle_nvn.c`) answers "already signaled"; it is off for every other game, since none has been checked with it.
- The game's own log (`h_diag_Put`) repeats one line about a UDP socket every frame. A repeated line is now logged 20 times, then once every 10,000 with its count (`hle_sdk16.c`), so the rest of the log stays readable.

## Rendering

- **Resolved: flickering black holes and shards** (the Mac too, more in some views than others). The depth pre-pass draws each mesh through geometry shader `38d8127afde928b3`, which culls small triangles (8 pixels or less) against the live depth buffer and appends the visible triangles' 16-bit indices to a list (ballot, atomic add and a 32-bit compare-and-swap on the word that holds each half). The depth-equal colour pass then draws that list indirectly. The lists start at 2-byte boundaries, and Metal's word accesses landed 2 bytes early, scrambling the indices. With `shader_sb_unaligned on`, the translation counts storage offsets from the binding's address rounded down to 16 bytes, and the replay binds the buffer there ([Storage buffers at addresses that are not word-aligned](../../../docs/reference/shaders.md#storage-buffers-at-addresses-that-are-not-word-aligned)). Ruled out on the way: GPU overlap between passes (`DM_NVN_PASSGPU_SERIAL=1` was still broken) and the depth convention.
- 863 shaders: GLSLC outputs in the renderProg entries of the `.resources` files, found through the newest `.pindex` (`shader_split.py` `idtech_shaders`: a per-entry data file number, raw deflate without a final block).

## Performance

- **The mesh path.** The pre-pass's culling geometry shaders read clip z, and their draws are triangle lists with primitive restart on but no restart index. The mesh composer now passes clip z (the GL entry gets `2z − w`, `dmsh_mesh.py`), and `nvn_geometry.inc` lets such lists take the mesh path. With the pairs in `geo_mesh.tsv`, a gameplay frame on the Mac went from about 485 render passes and 1.2 GB of attachment traffic to 73 passes and about 170 MB, and its GPU span from about 28 ms to about 7 ms. On the iPhone at 30 fps, late frames went from 33–54 in 300 to 0–5, and the phone runs noticeably cooler. The pairs come from pass dumps of the first levels; a draw whose pair is not listed logs `has no mesh library` (the first 256), and its pair belongs in `geo_mesh.tsv`.
- **Where the GPU time goes** (iPhone, serialised per-pass times, 15–19 ms a frame): the main pass 3–6 ms, the 4096² shadow map 1.5–2.5 ms, the depth pre-pass 0.7–1.7 ms. The rest is the game's own draws, not host overhead.
- **Tried and rejected** (each an A/B in one session, alternating 300-frame phases: separate runs are not comparable, since the AI plays out differently):
  - shading each distinct index once per mesh threadgroup in the mesh path: the pre-pass took about twice as long (its vertex shaders are cheap);
  - skipping the shadow map's load: no change (lossless compression is already on);
  - the 4-tap depth downsample (`81b2c4ade116834b`) looked slow only in a frame outside gameplay.
- **Resolution.** The game's dynamic resolution (`rs_*` cvars) logs `GPU msec: 0.0` and never scales: in gameplay it calls neither `nvnCommandBufferReportCounter` nor `nvnDeviceGetCurrentTimestampInNanoseconds`, and `DM_NVN_GPU_TS=segment` changes nothing. `+set rs_enable 1 +set rs_forceFractionX 0.5 +set rs_forceFractionY 0.5` renders the 3D scene at 640×360, but the Mac's GPU time only went from about 7.2 to about 6 ms, with a soft picture: most of the cost is not per pixel. Not tried on the iPhone.
- 60 fps (`DM_PRESENT_INTERVAL=display` or `adaptive`) runs, but with uneven frame pacing on the iPhone, where 60 falls to about 25 in heavy scenes; 30 fps is the default.

## Controls

- The game takes the on-screen controls as a Pro Controller. Its stick dead zone (`joy_deadZone`) is 0.35, so the touch sticks feel slow to start: on the iPhone Duo's inner display, `DM_TOUCH_STICK_FLOOR=0.36` and `DM_TOUCH_STICK_FULL=0.5` in `Documents/env.txt` start the left stick past the game's dead zone and reach full tilt sooner (`DmGamepad.m`).
- **Touchpad aiming** (`DM_TOUCH_AIM`, a launcher toggle): the right half of the screen drives the right stick by the finger's speed, with the AIM button's sensitivity slider ([settings](../../../docs/settings.md#touchpad-aiming)).
- **Gyro aiming** (`DM_PHONE_FLAT`, a launcher toggle): the game takes its gyro yaw from z alone for a Pro Controller (`main+0x1d0f124`) and from y + z in handheld (`main+0x1d0f148`). Docked, turning the upright phone (about y) barely turned the view; the toggle hands the phone's motion over as a Pro Controller held flat (`hle_hid.c` `six_flat`).
- ZL / L / ZR / R and L3 / R3 sit just above the bottom controls in the 3D layout, where the thumbs reach them (the glory kills and the use button are L3 / R3).

## Open

- The intro movies and warnings cannot be skipped: `com_skipIntroVideo` is ignored and the warnings are timed.
- `pso_manifest.tsv` is empty: the pipeline seed list has not been recorded.
- Levels past the first ones may draw culling pairs that `geo_mesh.tsv` does not list (see [Performance](#performance)).

## Mac routes

- Game arguments go after `--` on the host's command line, such as `-- +set rs_enable 1`. The profile has no `game_args`.
- The in-game EULA needs ticking once on a fresh save.
