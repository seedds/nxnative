# Kirby and the Forgotten Land (`01004D300C5AE000`)

Update 1.1.0 (v65536) over the base game, SDK 11.4.0. HAL Laboratory's own engine on NintendoWare (G3d, Font, Fxt) with `nn::atk2` sound and NEX online. Modules `main`, `subsdk0` (NintendoSDK_movie 11.4.0) and `sdk`; no other profile ships these `sdk` and `subsdk0` builds. Plays on the Mac at 60 fps (30 on the Switch): the online and difficulty prompts, the 1080p opening movie, the beach and the walk through Point of Arrival with its guide movies, the reveal of the city, inhaling a Blade Knight for the Sword ability, fights with Awoofies and Bronto Burts, the title screen and continuing a file. Plays on the iPhone at 60 fps (`fps_target 60`; 30 in the settings keeps the game's own limit).

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB block: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | no system resource; 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region. See [Memory](#memory) |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk0` is the movie library: the opening and ending movies and the guide movies |
| `shader_groups /gfx/=gfx /ptcl/=ptcl /lyt/=lyt /shader/=shader` | `game.tsv` | the shaders are BNSH archives in HAL's `.cmp` containers; see [Files and shaders](#files-and-shaders) |
| `fps_target 60` | `game.tsv` | HAL's frame interval freed and the dynamic resolution's bounds scaled, at the places `dm/tools/hal_fps.py` finds by the engine's own code in any build that keeps it (`game` binding); see [Frame rate](#frame-rate) |
| Node area cut and 2 MB alignment of large regions (SDK 20's) | `dm/tools/sdk_patches.py` SDK-wide rules, `sdk` build `b39add63…` | [Pokémon Legends: Arceus](../01001F5010DFA000/README.md)'s build, cut by the SDK-wide rules; this game has no virtual address memory and never reaches them |

No code of the game is cut. The game asks for present interval 1 and paces itself at 30 fps; at 60 the host frees its frame interval ([Frame rate](#frame-rate)).

## Memory

- The NPDM declares no system resource, so there is no virtual address memory: the game sets a heap of the pool less the used memory, 3.14 GB (`[mem] heap 3.14 GB (heap region 3.25 GB)`), which the 3.25 GB heap region holds.
- **Current:** 4.1 GB resident and 2.67 GB of Metal allocations on the Mac after 20 minutes in Point of Arrival (`footprint`: 2.6 GB untagged, the guest memory the game touches; 0.7 GB graphics; 0.6 GB large host mallocs). It grew with each new area (3.2 GB at the beach, 3.97 GB at the city) and then stayed nearly flat, 4.13 → 4.17 GB with Metal allocations steady at 2.67 GB, over a 15-minute soak in one area. The highest resident figure of the games so far; how much a phone allows is open.

## Files and shaders

- RomFS: 9,386 files, 5.72 GB. 7,201 `.cmp` and 430 `.cmpbin` containers (HAL's size-prefixed zstd and LZ4 frames), 52 `.bfstm` and 104 `.opus` streams, 36 WebM movies (`mov/EventMovie`, `mov/GuideMovie`).
- 27,297 shaders in the map (18,460 pixel, 8,755 vertex, 72 compute, 10 geometry): 18,476 in `gfx/`, 8,313 in `ptcl/` (70 of them compute), 387 in `lyt/`, 67 in the loose `shader/` files and 54 in BNSH archives embedded in `main`. Before the split opened the `.cmp` containers it found 121 ([HAL's `.cmp` containers](../../../docs/reference/shaders.md#hals-cmp-containers-a-size-prefixed-frame)). All of them built for the Mac without a failure, and no run met a shader missing from the map (`shader keys not in the shader map … 0`).

## Rendering

- **Current, harmless:** `[RED] texture type mismatch: shader … declares slot 1 / 2 as Metal type 2, texture ID … is type 7 (NVN target 2, format 0x38 …); bound as is`, once per shader, for eight UI layout pixel shaders so far (`lyt/TitleMenu`, `lyt/Resident/TitleLogo`, `lyt/Cmn/HeroPanel`, `lyt/Step/ScopeCine`, `lyt/Arena/Entry`). The texture is a 16×16×16 RGBA8 sRGB 3D texture, one of the colour-grading lookup tables (`texlist 2`), left in slots the layout material does not use: the layout shader samples slots 1 and 2 only when its uniform `c3[0].y` is non-zero.

## Movies

- The SDK's demuxer (libwebm in `subsdk0`) runs natively; the host decodes VP9 with libvpx and Vorbis with stb_vorbis into the game's CPU buffers (NV12, mode 1). The opening movie is 1920×1080 VP9 with a Vorbis track (2,297 frames); the guide movies that show a move in a corner of the screen are 480×270 VP9 without sound.
- **Resolved: the game froze at the first guide movie** ("A Jump", on the first walk from the beach, every time). A job worker (priority 46) returning an output buffer signalled the movie's events while the host decoder held its decode lock; the signal handed core 0 to the movie threads (priority 44), and the input thread among them was waiting for that lock in host code, which the handoff counted as running ([A host lock held across a same-core handoff](../../../docs/reference/threads.md#a-host-lock-held-across-a-same-core-handoff)).

## Frame rate

- **Current:** 60 fps (`fps_target 60`); 30 in the settings or `DM_FPS_TARGET=30` keeps the game's own limit, as on the Switch (on the Mac: `power` lines 28–30 fps, frame interval p99 33.6 ms in play, CPU about one core and 5–5.6 W on an Apple M6 in Point of Arrival).
- **How:** HAL's frame interval is an int32 in its frame object, `[main+0xa76728]+0x50` on 1.1.0 (the constructor stores the object's interface there, at `main+0x526560`, and clears the field; the game then sets 2). `hle_fpslock.c` writes 0 above 30, which lifts the game's limit and steps it by the time that really passed, so the host's present cap sets the rate. The dynamic resolution's GPU frame-time bounds, two floats at `[[[[main+0xa767b0]+0x140]+0x1b0]+0x20]+0x34` (1.95 and 1.7, set for 30 fps), are scaled by 30 / 60. The rewriter finds both by the engine's own code, with no build ID (`dm/tools/hal_fps.py`; main's fixups `HAL_*`): the frame manager's switch on the field, the constructor that stores the object, and the engine's heap roots ([A vsync interval of 0](../../../docs/reference/frame-rate.md#a-vsync-interval-of-0-no-limit-and-a-step-of-the-time-that-passed)). On the 1.0.0 main it finds `0xa72628` and `0xa726b0`, the places masagrator's FPSLocker config lists for that build (the source of these places; the interval found by Hazerou). The log shows `main: HAL frame interval 2 → 0 (60 fps)` and `main: HAL dynamic resolution GPU frame time max 1.95 → 0.975`.
- **Ceiling:** 60. The frame manager (`main+0x532eb0`) counts each frame's step in whole 60 Hz vsyncs of 16,666,666 ns, at least one (`cmp w8, #1; csinc`), so a shorter frame would run the game fast. On a 120 Hz display link an iPhone 17 Pro Max refreshed at 120 and the game still delivered 60 frames a second, at 3.5 times the CPU energy (3.4–3.8 W). So the profile leaves `fps_max` at 60, the default: `DM_FPS_TARGET=120` runs it at 60.
- **iPhone 17 Pro Max, iOS 27.2, 8 minutes of play (28,800 frames):** median 59.8 fps over the `power` windows; 57 of 96 `on-screen` windows with p99 16.67 ms. The dips (44–50 fps in the worst windows) come with bursts of new shaders and textures as areas load, not with heat: at thermal state 2 the game holds 60 between them. CPU 1.2–2.5 cores, CPU energy 0.2–1.0 W after the first loading (3 W), footprint 2.75–4.0 GB; thermal state 1 after 79 s, 2 after 349 s. The host's 30 fps thermal cap (`thermal_capped`, [A thermally serious iPhone holds 30 fps](../../../docs/reference/frame-rate.md#a-thermally-serious-iphone-holds-30-fps)) covers the frame step only, so an engine limiter such as this one stays at 60 when the device is serious.
- **Mac, Apple M1 Max, macOS 27.2, 3 minutes of play (11,700 frames, Core Audio output):** 60.0 fps in every `power` window but the first, of loading (53.9); 35 of 38 `on-screen` windows with p99 16.67 ms. CPU 1–1.7 cores, CPU energy about 2 W (3.8 at most), 3.6–4.8 GB resident; the game exited cleanly.
- **Speed:** the same scripted walk (`DM_CTL`: `LUp~1.5` from the spawn point in Waddle Dee Town, facing the World Map star) took Kirby to the same place at 30 and at 60 fps on the Mac, where a game stepping twice as fast would have covered twice the distance; the game and its cutscenes also ran at the right speed to the eye on the iPhone (the player's report).
- The iPhone, Mac and speed runs above were made with a host row for these same places on build `a6ce40dc…`; the rewriter's places for that build are the same (its fixups, 2026-11-09), and the run through the fixups is not yet repeated in game.

## Audio

- `nn::atk2` through an audio renderer (96 voices, 14 effects, one sink). 131,543 of the first 132,000 rendered frames were not silent.
- **Current, open: with the silent sink the game often crashes right after Continue.** On the Mac's default output (no `DM_AUDIO`, or `DM_AUDIO=0` as `regress.py` runs it), `A` on the title's Continue crashed 7 boots of 11 within 4 s, during the load: `CRASH sig=11 addr=0 pc=main+0x6da824 lr=main+0x6dabf8`, on a job worker (priority 46, backtrace `main+0x6dabf8 main+0x803cd4 main+0x6a69e4 main+0x6d9e80 main+0x78a5b8`), with Continue pressed at 20 s or 26 s, an empty or a full pipeline archive, this host or the one the game was brought up with. With `DM_AUDIO=1` (Core Audio output) 3 boots of 3 reached the beach. The cause is not known: what the null pointer is and why the silent sink's pacing exposes it (2026-10-27).

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`; its last line runs the game.
- A fresh save, through `DM_CTL`: the online prompt is up about 20 s after boot: `Left`, `A` (Do Not Use), `A`; the difficulty: `Right`, `A` (Spring-Breeze Mode). The game saves here, then plays the opening movie and the beach scene (about 75 s) and gives control on the beach.
- With that save, the title shows Continue about 25 s after boot; `A` puts Kirby on the beach. Forward (`LUp`) leads to the first guide movie in about 15 s; the path then turns right past a rock, crosses a ravine under a plank (float up on the right with `A`) and runs forward between trees to the city reveal, then right past the building's base to the first Blade Knight.
- A `DM_CTL` press replaces the one being held: send the next key only after the hold ends.
- `smoke.tsv` (for `scripts/regress.py`) starts from that save, kept as `saves/beach` in the NSP folder. Run it with `--env DM_AUDIO=1` until the [crash after Continue with the silent sink](#audio) is fixed.

## Testing

- 2026-10-26, Mac: boot, the online and difficulty prompts, the opening movie, the beach, Point of Arrival with three guide movies, the city reveal, inhaling a Blade Knight (Sword), fighting with the Sword, losing it to damage, the title screen and Continue, over five runs (the longest 30 minutes, with a 15-minute soak of walking, jumping, floating, inhaling and attacking, no crash). The only red lines were the layout shaders' [texture type mismatches](#rendering); a key dump of the title menu's shader (`DM_NVN_KEYDUMP=5de6e5dc52a56300`) shows `c3[0].y` = 0, so its reads of slots 1 and 2 do not run.

## Open

- The rest of the game is untested on the Mac: no stage was finished, so no Waddle Dee Town, boss or Mouthful Mode yet, and no save at a stage's end.
- iOS: tested on an iPhone 17 Pro Max with the Increased Memory Limit capability (footprint up to 4.0 GB at 60 fps, above the 3.4 GB iOS gives an app without it); 8 minutes of play logged.
- 60 fps through the fixups: a run in game of the rewriter's places (the measurements in [Frame rate](#frame-rate) used a build row for the same places).

## Found on this game

- [HAL's `.cmp` containers: a size-prefixed frame](../../../docs/reference/shaders.md#hals-cmp-containers-a-size-prefixed-frame).
- [A host lock held across a same-core handoff](../../../docs/reference/threads.md#a-host-lock-held-across-a-same-core-handoff).
- [A vsync interval of 0: no limit, and a step of the time that passed](../../../docs/reference/frame-rate.md#a-vsync-interval-of-0-no-limit-and-a-step-of-the-time-that-passed).
