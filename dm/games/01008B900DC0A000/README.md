# FINAL FANTASY VIII Remastered (`01008B900DC0A000`)

Update 1.0.1_5 (v196608) over the base game, SDK 7.4.0, BaseEngine. The ExeFS loads `main`, `subsdk0` (GLSLC), `subsdk1` (NintendoSDK_movie) and `sdk`; `subsdk2` is NVIDIA's OpenGL driver and is not loaded. The current Mac build plays through the intro and the first playable section. iOS has not been built or tested.

## Files and modules

- `main` is a small loader (3,712 instructions, 107 imports, all from `sdk`). The game is 14 NROs that 14 NRRs in the RomFS register, loaded with `nn::ro`: Game, Resource, UserServices, AudioSystem, GuardianEFIGS and GuardianJP (about 40 MB each), Mp4Player, Font, Math, Platform, System, Memory, Rendering and Multithreading. All are loose `.nro` files in the RomFS root. The NROs import from `subsdk0` and `subsdk1`, which `main` does not, so `modules` lists both.
- **Resolved: SIGBUS at the first `LoadModule`.** Math.nro is text, ro and data of 4 KB each, and Platform.nro has data at `0x3000`: both had data on the code's 16 KB page, and the game stopped at `CRASH sig=10 addr=nro8+0x2180 pc=relocate+0x178` during the NSP script's GLSLC capture. `patch_nso.py` (`dm/tools/mod_layout.py`) now moves their ro and data up when rewriting. [Module data on the code's 16 KB page](../../../docs/reference/memory.md#module-data-on-the-codes-16-kb-page).
- The RomFS holds 12,824 files, almost all under `weepff8/game_data` (12,181 `.ddsz` textures, 109 `.mp4` movies, 87 `.ogg`).

## Memory

The NPDM declares no system resource. `main` sets a heap of at most the pool less 64 MB, so the profile keeps the survey's 3,328 MB heap region (`mem_reserve 0x100000000`). Peak use in later areas has not been measured.

## Rendering and shaders

- Rendering.nro and Platform.nro draw with OpenGL ES through EGL. Rendering.nro and the two Guardian modules also call `nvnBootstrapLoader` and compile their GLSL at run time with `glslcCompile` from `subsdk0`, so the NSP script boots the game on the Mac to capture what it compiles.
- The 30 s capture run reaches its end at about 60 fps with no crash and records 34 shaders: 18 vertex and 16 pixel. These are the profile's `shader_map.tsv` and `shader_pairs.tsv`.
- **Open: shaders compiled after the capture.** No missing shader was reported through the first playable section, but later areas, battles or menus may compile shaders the 30 s run does not reach. A run with `DM_GLSLC_CAPTURE` collects them.

## Mac testing

On 2026-11-21 a Mac build played through the intro and the first playable section with no `DM_` switches.

## Found on this game

- [Module data on the code's 16 KB page](../../../docs/reference/memory.md#module-data-on-the-codes-16-kb-page): Math.nro and Platform.nro.
