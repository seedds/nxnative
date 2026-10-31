# SteamWorld Heist II (`010089C01E9CA000`)

Update 1.5.1 (v262144), SDK 17.5.6, on NVN (79 shaders, GLSLC programs). Modules `main`, `subsdk0` (`NintendoSDK_movie`) and `sdk`. Thunderful's own engine (the SteamWorld series' engine, from Image & Form), with its shaders, scripts and data in two zip `.pak` archives in RomFS. The standard host and rewriter run this build with no game-specific fix: no shader overrides, rewriter cuts or host changes. The profile keeps the NSP script's provisional memory layout and records the observed depth mode ([What it needs](#what-it-needs)). Only this update is registered in the profile.

On the Mac (2026-10-29, a short gameplay check): the Thunderful logo, the title menu, a new game (Experienced, with the tutorial), the 80 s intro movie, then the first mission (Royal Navy Shipyard): its dialogue, the move cursor and a move, at 60 fps (frame time p50 16.67 ms, GPU p50 about 8.5 ms at 1080p) with audio and no `[RED]` line. In a second run the start menu listed the save (Game 1, 00:01:01, Find Leeway's Submarine) and Continue Game put the crew back in the mission, so the save and its reload work. The unsigned IPA is built with the frame rate counter but not tested on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | `game.tsv` | the game selects near-zero NVN depth; offline pipeline bases must use the matching vertex entry |
| 3.25 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout, which holds: `main` sets a heap of at most the pool − 64 MB |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `modules main subsdk0 sdk` | `game.tsv` | `main` imports the movie decoder from `subsdk0` |
| `shader_groups /Core/=core` | `game.tsv` | every shader is in `Core/Game2.pak` (`Shaders/NVN/*.nvns`) |
| `pad_layout` (default, `simple`) | | the 2D virtual pad; the left stick moves the cursor, A confirms |

## Files

- RomFS: 1,903 files, 1.20 GB, all under `Core/`: `Audio/` (1,065 WAV and 123 Ogg files, 477 `.bnvib` rumble files), `Atlases/` and `Textures/` (KTX), `Characters/` (`.isc2`), `Fonts/` (`.fgen`), `Movies/` (7 WebM files) and the two zip archives (`.pak`, 0.05 GB), which hold the shaders.
- The save holds `Save000/savegame_000.dat`.

## Shaders

- **Resolved: offline depth metadata.** Update 1.5.1 selects near-zero NVN depth in `run/r3.log` (through frame 2,400), with no later GL switch. A short isolated boot reproduced the call with matching registered module IDs; evidence is in `010089C01E9CA000-work/depth-mode-audit/boot-20261031/` beside the NSPs. The profile now declares `depth_mode zero` for the [shared archive mechanism](../../../docs/reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree). This confirms the input; rebuilt archive acceptance and performance were not retested.
- **Current:** 79 distinct shaders from the `.nvns` GLSLC files in `Core/Game2.pak`, all translated for the Mac.
- **Open:** the prewarm list (`pso_manifest.tsv`) is empty. On the first Mac boot, before the build had a pipeline archive, draws waited for pipelines a few times: 20–95 ms on the title and the menus, and one frame of 399 ms when the first mission loaded (12 draws, 350 ms). A run after the NSP script's prebuild found its bases in the archive. A list recorded through the title and the first mission would spare the iPhone's first start those waits.

## Movies

- **Current:** the logo and the intro (`Core/Movies/*.webm`) are VP9 1080p, decoded by `movie::Decoder` into the game's NVN texture (mode 2), through [VP9 on host threads with libvpx](../../../docs/reference/movies.md#vp9-on-host-threads-with-libvpx) and [Decoding into the client's NVN texture](../../../docs/reference/movies.md#decoding-into-the-clients-nvn-texture). The intro's 2,377 packets took 5.9 ms each on average to decode. It plays to its end; A, B and + do not skip it.
- **Open:** while the intro plays, about one frame in four takes 23–29 ms on the CPU (frame p95 24–30 ms, GPU under 1 ms), against 16.7 ms elsewhere; each such frame re-uploads the 7.9 MB movie texture. Where the time goes (the CPU conversion into the texture, or the game waiting for an output) is not measured.

## Memory

- Resident memory was about 750 MB on the title and 2.2–2.3 GB in the first mission; Metal allocations up to 1.8 GB.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game; with the window covered add `DM_PRESENT_LINK=0` so `DM_CTL` screenshots still come.
- With a fresh save: the title menu is up about 20 s after start; A (Start), A (New Game), A (difficulty), Y (Start Game), A (Yes, tutorial) runs the intro, and the mission starts about 80 s later. The intro cannot be skipped.

## Found on this game

- **Offline depth metadata:** [Offline pipeline archive and profile depth disagree](../../../docs/reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree).

## Open

- iPhone: the IPA is built but not tested.
- The intro movie's long frames ([Movies](#movies)).
- Only the start of the first mission was played; combat, the ship, the world map and the other languages are untested.
