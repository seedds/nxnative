# Atelier Ryza 2: Lost Legends & the Secret Fairy (`01009A9012022000`)

Update 1.0.7 (v458752, patch title `01009A9012022800`) over the base game, SDK 9.6.0: the same `sdk` module build and `subsdk0` (NintendoSDK_movie: the opening movie) as [Hyrule Warriors: Age of Calamity](../01002B00111A2000/README.md). Gust's engine on Koei Tecmo's KTGL (`KtglFsThread_Host`, `KTSL2` sound, KIDS tasks) on NVN. 24 DLC packages. Plays on the Mac at 30 fps, as on the Switch: the Koei Tecmo logo, the title, a new game through the opening cutscenes into the first field, a load of its autosave, walking, the camera and the main menu. An unsigned IPA has been built; iOS is untested.

Started from the NSP script's provisional profile: it booted and drew the title on the first pass with no key beyond the guessed ones, so the profile adds no host-behaviour key beyond what the survey wrote (only `pad_layout` and `zip_store`).

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource (META +0x14 = 0): no virtual address memory; `nninitStartup` sets a heap of 3.14 GB in the 3.25 GB heap region (the layout of [Atelier Ryza: Ever Darkness & the Secret Hideout DX](../0100199023B2E000/README.md)) |
| `pad_layout full` | `game.tsv` | a 3D game: the right stick turns the camera, the D-pad opens the field map |
| `zip_store .gz:...` | `game.tsv` | the 6,037 `.gz` files are already compressed |

No code of the game is cut and the profile has no `patches.py`: with `sys_resource 0x0` the game uses no virtual address memory (so the `sdk` module's address-region cuts in `dm/tools/sdk_patches.py` are not exercised), and the game needs no run-time key. The game sets present interval 1 and 2 by scene and paces itself to 30 fps either way; the profile sets no `fps_target` (see Frame rate and memory).

## Files and shaders

- RomFS: 17,898 loose files, 6.87 GB, 6.81 GB of it under `Data/NX`: 6,037 `.gz` archives (4.4 GB; Gust `CRAE` containers and Gust's chunked zlib), 1,904 G1T textures (1.2 GB), 363 G1S shader files, KTSL2 sound banks (VOICE 322 MB, BGM 207 MB), three MP4 movies (the opening 147 MB per language, the ending 256 MB), event scripts, XML tables. `romfs_marker` is `Data/NX/Sound/VOICE.ktsl2stbin`. The host reads it as `fs RomFS image ...: 19179 entries`.
- 1,556 shaders (834 pixel, 598 vertex, 122 compute, 2 geometry), all built for the Mac, from the same sources as the DX: the G2S containers of `Data/NX/Shader/*.g1s`, `.fpo` / `.cpo` files, the elixir archives ([Gust's chunked zlib `.gz` files](../../../docs/reference/shaders.md#gusts-chunked-zlib-gz-files)) and 54 in `main`. The vertex shaders are single-stage programs, paired with a cover pixel shader. No shader was missed (no `[RED]` line over the opening and the first field).
- DLC: the NSP script wrote one RomFS image per DLC with data to `aoc/<index>.img` (indices 200-205, 207-214, 300, 301, 400, 401, 600-602, 700-702; the licence-only packages have no data), the host reports 24 add-on contents and mounts each (`fs mount aoc200: ...`). No DLC file holds a shader.

## Frame rate and memory

- **Current:** 30 fps on the Mac (M4 Pro, headless): frame time p50 33.4 ms and p95 34.8 ms in the field (frames 12,000 and later, 23 windows of 300 frames), GPU p50 7.2 ms (7.1 to 7.5) and p95 10.1 ms; in the title and the cutscenes GPU p50 6.4 to 11.2 ms. The profile sets no `fps_target`: this build has neither a frame step nor an engine limit the host writes, so a cap would only log `present cap … not applied` ([A present cap applies only with a frame step or an engine limit the host writes](../../../docs/reference/frame-rate.md#a-present-cap-applies-only-with-a-frame-step-or-an-engine-limit-the-host-writes)), and the game's own present interval stays.
- **Frame rate 60 is not built.** The game steps per presented frame, so 60 needs its update step changed:
  - The pacing is the present interval, not a limiter of the game's own: it sets interval 1 or 2 by scene (`main+0x744950` sets the KTGL device's interval, called at `main+0x9f262c` from a scene's mode object: +0x1d0 forces 2, +0x1d4 holds the wanted one, clamped to at least 1; the device keeps it at +0x2a9c and passes it to the window at `main+0xba1ac0`), and the host in `present_interval game` mode holds each frame that many display ticks. No 33.3 or 16.7 ms constant is built from immediates in `main`.
  - **Probe (Mac, `DM_PRESENT_INTERVAL=display DM_FPS=60 DM_FPS_TARGET=60`, one run):** the game presents at 60 fps, frame time p50 16.67 ms, p95 17.3 ms, GPU p50 8.1 ms (p95 9.2 ms) over the opening, the title and the field, so the GPU would fit 60 on the M4 Pro. But the game's own time follows the frames: the opening movie and the title came at the same frame counts as at 30 fps (the title at about frame 10,000 in both), so everything ran at twice the speed. Not shippable as it is, so the profile keeps game mode.
  - The step: the float 1/30 (`main+0x115e94c` in the read-only data) has 17 readers (code at `main+0x65e50`, `0x662d8`, `0xb3050`, `0x307170`, `0x42f7c8` to `0x42fa1c`, `0xa29280` to `0xa29664`, `0xb04664`), 1/60 (`0x115ed7c`) 18; the KTGL device's frame rate 59.94 is made at `main+0x744118` (as in [Age of Calamity](../01002B00111A2000/README.md#frame-rate)). `cloth60FPS` (`main+0x45b8e0`) names a cloth mode of the engine. Open: which of the 17 readers step the game (an update dt per system, the event and movie timelines) and a stub that halves them at a 60 cap, with the interval forced to 1; an hour of static work did not find a single dt.
- Memory on the Mac: footprint 4.4 to 4.5 GB (resident 4.4 to 4.6 GB) in the opening and the field, Metal allocations 1.5 to 1.9 GB of it; the heap's 3.14 GB is only partly touched (resident 3.7 GB at frame 300, 4.4 GB by frame 5,400, then flat). This is above the DX's 4.0 GB; whether an iPhone with the increased memory limit holds it is untested. A smaller `app_pool` (the game sizes its heap from it) was not tried.
- The prewarm list (`pso_manifest.tsv`, 1,477 lines) was recorded over the logo, the title, the opening and the field.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`; its last line runs the game.
- **From a fresh save:** the title ("New Game", "Settings") is up at about 50 s (frame 1500). `DM_KEYS=52:A,56:A*450/1` starts a New Game and plays the opening (A at every line) into the first field on the dock of Royal Capital Ashra-am Baird (the tutorial "Press ... to bring up the Field Map" at about frame 15600, about 8 minutes). The game autosaves there.
- **With that save** the game shows its autosave notice (frame 900) and then the opening movie (`opening.mp4`, about 5 minutes, frames 1200 to 10000, which no key skipped); the title follows at about frame 10000 with "New Game", "Load Game" (highlighted) and "Settings". The same keys (`DM_KEYS=52:A,56:A*450/1`) press "Press any button" and Load Game, and the autosave puts the player back in the field on the dock with the HUD at about frame 13500 (a run of 475 s). Frame-anchored: `DM_KEYS=f10100:A,f10250:A,f10400:A,f10550:A,f10700:A,f10850:A,f11000:A` loads the save (field at about frame 12,500), then `f12800:LUp~f120,f13050:RRight~f60,f13200:R+LUp~f90,f13500:X,f13650:Down,f13750:B,f13900:B` walks forward, turns the camera and opens and closes the main menu.
- In the field the left stick walks relative to the camera, the right stick turns it, X opens the menu (Basket, Equipment, Items, Formation/Status, Guide, Options), B closes it.

## Testing

- 2026-11-25, Mac (M4 Pro, headless, no audio out, other builds sometimes running on the same Mac), a host from main 5dfc86ae with local host changes that are not part of this profile, shader chain 4f801d59044f:
  - the first pass of the NSP script with no profile: boot, the logo, the title, the opening and the field, 0 `[RED]` lines, 16,200 frames in 470 s;
  - New Game through the opening into the field on the dock (route above), then a load of the autosave from the title, walking, the camera and the main menu (frame-anchored keys): 0 `[RED]` lines, frame p50 33.4 ms, p95 35.8 ms, GPU p50 6.9 ms in the field (windows from frame 12,000), footprint 4.5 GB;
  - the 60 fps probe under Frame rate and memory;
  - 24 add-on contents reported and mounted (`fs mount aoc200:` ... `aoc702:`); the DLC menu was not opened.
- 2026-11-25, Mac (M4 Pro, headless, no audio out; another game's build and the tool tests ran beside it), this profile in `dm/games` on main eed6ffc9, shader chain 343aa69bfb7a: `scripts/build_ipa_from_nsp.py --platform mac` extracted the game and its 24 DLC, rewrote the three modules, and the split matched these tables (1,556 shaders, all built for the Mac in 38 s); then New Game from a fresh save (`DM_KEYS=52:A,56:A*450/1`, 560 s, 17,100 frames) through the opening into the field on the dock, the Field Map tutorial at frame 15,700 and the field with its HUD at 16,100 (screenshots checked): 0 `[RED]` lines, `interval mode game`, no `present cap` line, audio `OpenAudioOut` and `Start`, 24 add-on contents mounted; frame p50 33.3 ms, GPU p50 5.0 ms in the field (frame p95 36.4 ms, contended); the prewarm list read (1,260 shaders, 209 render and 8 compute pipelines, no key missing from the shader map).
- Not reached: a battle, synthesis, the mainland, the DLC areas; sound on a Mac with audio out was not listened to.

## Open

- iOS: an unsigned IPA is built (`--romfs separate`, on main eed6ffc9: 4.8 GB, because the 24 DLC images are inside it; the Legacy BGM Pack alone is 2.9 GB), nothing is tested on a device. The Mac's 4.4 to 4.5 GB resident is close to what an iPhone allows a game.
- 60 fps: no mechanism found (above).
- Not reached: a battle, synthesis, the mainland, the DLC areas and menu; sound on a Mac with audio out was not listened to.
- Native widescreen (the 16:9 HUD) was not looked at.
- Only update 1.0.7 is registered. Other versions come from a derived profile.

## Found on this game

- The DX's findings (Gust's chunked zlib `.gz` files) carry over without a change; the cube-to-2D view was not needed here: the host's counter of texture type mismatches that no view bridges stayed at 0 over the opening and the first field (16,000 frames).
