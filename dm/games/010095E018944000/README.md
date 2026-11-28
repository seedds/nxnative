# Atelier Ryza 3: Alchemist of the End & the Secret Key (`010095E018944000`)

Update 1.6.0 (v524288; base game v0) over the base game, SDK 14.3.0. Gust's engine on Koei Tecmo's KTGL (`KTSL2` sound, KIDS tasks) on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie: the movies) and `sdk`. Eight DLC packages (six with data). Plays on the Mac: boot, the logos, the title, a new game through the opening dialogue, the prologue battle and the story scenes after it into the first free field (about 15 minutes on a scripted route), a load of the autosave written there, and walking in the field near the Pixie Forest. Runs at 60 fps with the Switch's 30 fps selectable. An unsigned IPA is built; iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource: no virtual address memory; `nninitStartup` sets a heap of 3.14 GB in the 3.25 GB heap region (the layout of [Atelier Ryza: Ever Darkness & the Secret Hideout DX](../0100199023B2E000/README.md); the survey guessed the same) |
| `pad_layout full` | `game.tsv` | a 3D game: the right stick turns the camera; main reads no touch screen, mouse or keyboard |
| `zip_store` with `.gz` | `game.tsv` | the 9,570 `.gz` files are archives that are already packed (see below) |
| `present_interval display`, `fps_target 60` | `game.tsv` | the game asks for present interval 2 (1 on the logos and the title) and steps by the time that passed, so frames can follow the display tick and the player's Frame rate (30 or 60) is applied by the present interval alone; see Frame rate and memory |

No code of the game is cut and the profile has no `patches.py`: the rewriter finds nothing in `main` that needs a cut, and the `sdk` module (build `2aef2cff…`, which other profiles share) is handled by the SDK-wide rules.

## Files and shaders

- RomFS: 23,519 loose files, 10.49 GB (25,329 entries in the host's image with the directories), nearly all under `Data/NX` (G1T textures, G1S shader files, 9,570 `.gz` archives, 456 KTSL2 `.sebin`, 65 MP4 movies), `Event/` scripts (English and Japanese), `Saves/` XML tables and text.
- 1,756 shaders (949 pixel, 726 vertex, 79 compute, 2 geometry), all built for the Mac: the split found them in the same places as on the DX (GLSLC outputs in the G2S containers of `Data/NX/Shader/*.g1s`, the `.fpo` / `.cpo` files, the elixir archives and 76 in `main`), with no profile key and no change to the shader tools. [Gust's chunked zlib `.gz` files](../../../docs/reference/shaders.md#gusts-chunked-zlib-gz-files) were already handled. The vertex shaders are single-stage programs, paired with a cover pixel shader.
- DLC: eight packages, indices 202-205, 207, 400, 401 and 700. The NSP script packs each one's RomFS as `aoc/<index>.img` (202: 71 entries, 203: 11, 204: 26, 205: 11, 207: 38, 700: 14; 400 and 401 are licences with two entries each) and the host reports all eight owned and mounts them (`aoc: reporting 8 add-on contents`). No DLC file holds a shader.

## Display, input and movies

- 1920x1080 window texture, as on the other KTGL games. The opening is H.264 video and in-engine cutscenes; the title, the logos and the battle's HUD look right (screenshots checked by eye, not measured).
- The game reads the six-axis handles but no touch screen, mouse or keyboard.

## Frame rate and memory

- **Current: 60 fps by default, 30 selectable.** The Switch runs at 30. The game asks for present interval 1 on the logos and the title (60 fps there even in game mode) and 2 elsewhere, and nothing else limits it. Its logic steps by the time that passed: the prologue battle's AP bar filled at 11 AP a minute in the 30 fps run and in the 60 fps run, over the same inputs (30 fps: AP 5 at 220 s, 16 at 280 s; 60 fps: AP 1 at 200 s, 12 at 260 s). `present_interval display` therefore gives 60 fps at the right speed, and `fps_target` 30 or 60 is applied by the present interval alone ([A present interval that is the game's only limit, in display mode](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode)). A community Switch 60 fps patch for this `sdk` build forces the same swap interval (it rewrites one `csel` of the `sdk` module's `android::Surface` interval clamp to `mov w8, #1`, `sdk+0x17f198`); the host's present interval replaces that code, so the profile key does the same job with no rewriter cut.
- Mac, M4 Pro, headless, 60 fps, frames 6000 on (opening dialogue and the prologue battle, five runs of 8 to 10 minutes): frame time p50 16.66 ms, p95 16.9 ms; GPU p50 6.5 ms (dialogue) to 9.6 ms (battle), GPU p95 about 11 ms. 30 fps over the opening: frame p50 33.3 ms, GPU p50 8.7 ms. Some runs overlapped other work on the Mac, so the GPU times are contended; the 60 fps runs differ by less than 1 ms in median from one another. Mac numbers, not phone numbers.
- In the field (Kurken Island, the path to the Pixie Forest, loaded from the autosave and walked for about two minutes, one run on main eed6ffc9 with an iOS build running beside it): frame time p50 16.6 to 16.9 ms, p95 17.3 to 19.4 ms, GPU p50 6.9 to 8.0 ms, GPU p95 7.4 to 10.1 ms.
- **Open:** a 50 ms frame every 20 s at 60 fps (3 frames, no new pipeline, GPU 5 ms: a game-side job, not found), and loads of 2 to 8 s at scene changes (frame 145 at the title; the game's own loading, GPU idle).
- Memory on the Mac: resident 3.4 GB at the title, 3.9 GB over the dialogue and the battle (steady from frame 900 to 35,000), of which 1.2 to 1.9 GB Metal allocations; 4.0 GB walking in the field after a load of the autosave (1.4 GB Metal), and 4.24 GB (2.16 GB Metal) when the field followed the opening in one run. The guest heap is 3.14 GB of the 4 GB block. Only the first part of the field was walked, so what an iPhone with the increased memory limit holds over the rest of it is untested.
- The prewarm list (`pso_manifest.tsv`, 2,020 lines: 1,697 shaders, 302 render and 21 compute pipelines) was recorded over boot, the title, the opening dialogue and the prologue battle, then a load of the autosave and the start of the field. Without it the first opening on the Mac hit 8.1 s (12 new pipelines) and 4.0 s (15) frames, and the first frame in the field 7.6 to 8.0 s (4 to 6 new pipelines, cold); with the opening's rows `pipeline prewarm done` took 1.5 s of background time on the Mac and the opening had no new-pipeline frame beyond 4 pipelines at frame 124. The field's rows were added last (16 rows); their effect on a cold first field frame was not measured again.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`; the first pass is about three minutes (survey, rewrite, split, 1,756 shaders, host) on an M4 Pro after the extraction, and the iPhone build (`--romfs separate`) another two.
- The title shows "Press any button" at about 20 s, then New Game, Prologue Movie, Settings. From a fresh save: A at 25 s and 30 s (New Game), then A every 1.5 s with Plus held 1.5 s every 9 s through the opening dialogue (about 110 s), then A every 2 s, Plus held 1.2 s every 10 s and X every 14 s through the tutorial battle (it advances only with those; A alone stays on its tutorial prompts). The battle ends at about 390 s and a story scene follows. A `DM_CTL` file with timed lines drives it (`A~0.15`, `Plus~1.5`, `shot <png>`).
- After the battle and the story scenes, A every 2 s and Plus held 1.2 s every 10 s go on until the game hands over control at the start of the field on Kurken Island, between 900 and 960 s from boot (the quest "An Alchemist's Days": go to the Pixie Forest), and writes its autosave; Plus there opens the region map.
- **With that save** the boot plays a movie of about two minutes before "Press any button" (about 140 s); the title menu then has New Game, Load Game (highlighted), Prologue Movie and Settings. `DM_KEYS=142:A,148:A,152:A,156:A,160:A,164:A` loads the autosave into the field (HUD at about 153 s); the left stick walks relative to the camera, the right stick turns it.
- Run with no extra switches: the profile sets display mode and 60 fps.

## Testing

- 2026-11-25, Mac (M4 Pro, headless, no audio), a host from main 5dfc86ae with local host changes that are not part of this profile, shader chain 4f801d59044f: boot, logos, title, New Game, the opening dialogue and the prologue battle on a beach, 0 red lines in six runs (up to 35,700 frames at 60 fps; 14,100 at 30 fps); the DLC images mount (`aoc: reporting 8 add-on contents`); the prewarm list read; the profile's keys applied (`interval mode display`, `present cap 60 fps … applied by the present interval alone`).
- 2026-11-25, Mac (M4 Pro, macOS 27.2.1, headless, no audio; another game's runs, an iOS build and the tool tests ran beside it), this profile in `dm/games` on main eed6ffc9, shader chain 343aa69bfb7a: `scripts/build_ipa_from_nsp.py --platform mac` extracted the game and its DLC, rewrote the three modules, and the split matched these tables (1,756 shaders, all built for the Mac); then:
  - New Game from a fresh save on the route above for 1,520 s (90,000 frames): the opening, the prologue battle, the story scenes, the start of the field between 900 and 960 s and the region map ("Kurken Island Area", fog over the unexplored parts), 0 red lines, `interval mode display`, `present cap 60 fps (game profile fps_target) applied by the present interval alone`, 8 add-on contents mounted, the prewarm list read with no key missing from the shader map; the game wrote its autosave at the field;
  - three loads of that autosave from the title (`DM_KEYS` above) into the field and about two minutes of walking near the Pixie Forest, with its Landmarks tutorial: 0 red lines; the frame times and memory under Frame rate and memory; one of them recorded the field's pipelines for the prewarm list, and the last read the merged list (2,020 lines; 1,697 shaders, 302 render and 21 compute pipelines, no key missing; the only frame with new pipelines was at boot, 4 in 74 ms, with the Mac's Metal cache warm from the runs before).

## Open

- Only the start of the field was played: a battle beyond the tutorial, synthesis (the alchemy menu), a manual save, the rest of the open world and the DLC areas were not run; sound on a Mac with audio out was not listened to.
- iOS: an unsigned IPA is built (`--romfs separate`, on main eed6ffc9: 218 MB with the 8 DLC images inside; Frame rate 60 by default, 30 selectable) and untested on a device. 60 fps is the default on the strength of Mac GPU p50 under 10 ms over the opening, the battle and the start of the field; a phone that cannot hold it takes the Frame rate row's 30.
- The 50 ms frame every 20 s at 60 fps and the 2 to 8 s loads at scene changes above.
- Native widescreen was not looked at.
- Only update 1.6.0 is registered. Other versions come from a derived profile.

## Found on this game

- Nothing new in the shared code: the DX's findings (chunked zlib, cube view, memory layout) carried over; a game that steps by time and has only its present interval as a limit needs only `present_interval display` ([A present interval that is the game's only limit, in display mode](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode)).
