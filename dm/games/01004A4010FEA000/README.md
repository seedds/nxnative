# Bayonetta 3 (`01004A4010FEA000`)

Update 1.2.0 (v131072) over the base game, SDK 13.3.2 (no other game's `sdk` build). PlatinumGames' own engine on NVN; Wwise sound through `AudioOut`. Modules `main`, `subsdk0` (the movie library, NintendoSDK_movie: WebM movies, VP9) and `sdk`. Languages: English, Japanese, French, German, Spanish, Italian, Russian, Korean, Simplified and Traditional Chinese. Plays on the iPhone (IPA 14.2 GB); 60 fps in play, 30 where the game asks for it (the title and loading screens).

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource (META +0x14 = 0): no virtual address memory; `nninitStartup` sets a heap of the pool − used memory (3.09 GB), which the game maps only as it touches it |
| `mem_used 0x7400000` (116 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory (the game's `SetHeapSize` asks for pool − used) |
| `pad_layout full` | `game.tsv` | the 3D gamepad layout: the camera is on the right stick, and the D-pad picks items |

## Services and SDK

- **Resolved: null read at `main+0xb8462c` right after the first two textures, called from `main+0x670eb0`.** Not the fault itself: `nnMain`'s start-up (`main+0x6705c0`) returns false when any of its 30 steps fails, the game then skips its main loop and goes straight to shutdown, and the shutdown walks a pool (`main+0x4efce70`) whose initialization (`main+0xb840d0`) came after the failed step. `DM_PROBE` on each step's entry (lldb was unavailable: developer mode off) showed the last one entered, `main+0x69c698`, the built-in shaders of `shader2.dat` (`hwfillcolor`, `hwshaderpf`, …), and probes inside it the step that failed: creating a shader from its object (`main+0xd0d998`). The engine checks each shader object's GPU code version (byte 6 major, byte 7 minor of the object) against the driver's range (`nvnDeviceGetInteger` 0x2E–0x31: newest major, oldest major, newest minor, oldest minor) and refuses newer code; the host reported 1.14, the table measured on Super Mario Odyssey's SDK 7 driver, and `hwfillcolor.vso` is 1.15 (the game's objects run from 1.9 to 1.16). The host now reports the translator's supported GPU-code range; see [Newest GPU code version reported by the driver](../../../docs/reference/nvn-objects.md#newest-gpu-code-version-reported-by-the-driver).
- Two NVN getters were red stubs: `nvnWindowGetCrop` (read back every few frames; before any `SetCrop` it is the whole first window texture) and `nvnSamplerBuilderGetLodClamp`.
- **Not a hang:** the first boot sat on the loading clock for about a minute, the CPU idle, with every file read done after 12 s: that run compiled its Metal libraries and pipelines on first use (cold cache). With a warm cache the title is up after about 15 s; what looked like a 40-second wait in the read log (`DM_FSLOG`) was the title screen itself, which reads nothing. Screenshots every few seconds (`ctl.sh snap` from a script) told the two apart.

## Rendering

- Shaders are single-stage programs: the engine makes a program of each object and binds them per stage at draw time, so each is paired with a cover pixel shader. It finds its built-in ones by name (`hwfillcolor.vso`, …).
- **Two domain shaders do not build** (`DS_Vanish05_*.dso`: `SPIRV-Cross threw an exception: Subgroup builtins are not available in this type of function`); the one-lane rewrite (`spv_single_lane`) covers vertex shaders only, and tessellated draws are skipped anyway.
- From the title to the prologue's first play the picture is right as far as compared (title scene, menus, the VP9 opening movie, the first real-time scene, play with the HUD). Two red lines, once each: a pixel shader declares a cube texture where the game binds a 2D one (the zero cube is bound), and a draw whose stage 0 reads cb1 with none bound.
- The spin-wait search takes 37 loops in `main` (35 before its store rule) ([development.md, Power](../../../docs/development.md#power)).

## Files and shaders

- 72 PKZ archives in the RomFS root (14 GB; `root_000.pkz`, `core_000.pkz`, `pl_000.pkz`, `st1_000.pkz`, `sound_000.pkz` … `sound_017.pkz`, …) of DAT archives, compressed per entry with Oodle Kraken or stored (`OodleKraken` 11,760 entries, `None` 5,159; `main` knows `ZStandard` too, `main+0xcbdf80` maps the three names to 0xc9, 0x65 and 0), beside 478 loose WebM movies in `movie/` ([01](../../../docs/agents/01-extract.md#compressed-romfs-content)). `romfs_marker` is `root_000.pkz`.
- `main` reads the archives only through Oodle's streaming decoder (`main+0xcbc698` creates one, `main+0xcbc7f8` feeds it 256 KB at a time into a 10 MB window), so `OodleLZ_Decompress` was never linked in and its name string is not there. The Mac host's Oodle tool mode now finds `OodleLZDecoder_MemorySizeNeeded`, `Create`, `Destroy` and `DecodeSome` instead (by the strings of Oodle's `oodlelzcompressors.cpp` and their order) and decodes each entry with them in `DM_OODLE_SERVE` (`hle_oodle.c`). `profile_guess.py` turns it on for any game with Kraken PKZ archives (`oodle_packs`). The shader split decodes every entry of every PKZ (8 workers, one Mac host each) in about 20 s; only `root_000.pkz` holds shaders.
- Shaders: 8,024 PlatinumGames shader objects in `root_000.pkz` (`shader.dat`, `shader2.dat`, `shadereff*.dat`, `autoshadereff.dat`): `VSO\0` / `PSO\0` / `CSO\0` / `HSO\0` / `DSO\0` (the stage), u32 compiler version, u32 control length, u32 control offset (0x18), u32 code length, u32 code offset. 6,424 distinct keys (4,288 pixel, 2,047 vertex, 78 compute, 6 domain, 5 hull) (`shader_split.py` `pkz_entries`, `pgdat_members`, `pg_shader`). All but two domain shaders build.

## Status on the Mac

- The boot, the title, a new game (difficulty, Naive Angel Mode, brightness, the auto-save notice), the VP9 opening movie, the prologue's first real-time scene and a minute of play in it, with sound.
- 60 fps in play (frame time p50 16.67 ms, p95 17.5 ms; GPU about 5 ms a frame on an M6), 30 where the game asks for it (the title and loading screens); about 2 GB resident and 3.1 GB of Metal allocations, flat after the first scene. The title is up about 15 s after launch with a warm shader cache. The prewarm list covers that route (678 lines).

## Mac routes

- The work folder's `run/ctl.sh` drives a run started with `DM_CTL` (`CTL=<file>` picks another run's control file), `run/step.sh <shot> <presses…>` presses and takes a screenshot, `run/snapseq.sh` boots and takes one every few seconds, and `run/route.sh <run>` boots on a fresh save and drives the route above: A at the title, A through the logo, A (Standard), A, B (closes the Naive Angel Mode help), A (Off), A (brightness), A (the auto-save notice), then Plus, Down, A skips the opening movie.

## Found on this game

General host changes first needed here; each applies to every game:

- **The NVN layer advertises support for the game’s GPU-code versions** (`hle_nvn.c`), `nvnWindowGetCrop` and `nvnSamplerBuilderGetLodClamp`: [Newest GPU code version reported by the driver](../../../docs/reference/nvn-objects.md#newest-gpu-code-version-reported-by-the-driver), [NVN calls that were red stubs](../../../docs/reference/nvn-objects.md#nvn-calls-that-were-red-stubs).
- **Oodle's streaming decoder in the Mac host's tool mode** (`hle_oodle.c`, `OodleLZDecoder_*`, `oodle_packs`) and the **PKZ / DAT readers**: [01, Compressed RomFS content](../../../docs/agents/01-extract.md#compressed-romfs-content).
- **PlatinumGames shader objects** (`pg_shader`): [PlatinumGames' shader objects](../../../docs/reference/shaders.md#platinumgames-shader-objects).

## Open

- Dotted patches on some surfaces (a grid every 4 pixels: the platforms breaking up in the prologue, a floor near the camera) look like the engine's dithered dissolve; not compared with the Switch yet.
- The two domain shaders and the two red lines above.
