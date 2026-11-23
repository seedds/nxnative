# Alien: Isolation (`010075D00E8BA000`)

Update v327680 over the base game, SDK 8.2.0. A Switch port by Feral Interactive (the NPDM publisher string is "SEGA EUR"); the survey finds Havok and Wwise in `main`. Modules `main`, `subsdk0` (the movie module), `subsdk1` (GLSLC) and `sdk`. `sdk`, the movie module and the GLSLC module are also builds of [The Legend of Zelda: Link's Awakening](../01006BB00C6F0000/README.md) 1.1.0, whose `subsdk0` and `subsdk1` are these two the other way round.

Brought up from the survey and provisional profile of `scripts/build_ipa_from_nsp.py`, on the Mac and, through Peanuts Native, on an Android handheld (Retroid Pocket Nova, Turnip T30). Both boot, reach gameplay and draw the 3D scene. On the Mac, in a 6.5 minute run from the opening room (before the last rebase onto `main`; the profile and its tables did not change, and the rebuilt game reached gameplay again): 30 fps, no dropped frame in the sampled 300-frame windows, footprint about 3.4 GiB and flat, no audio underrun. No IPA was built; the Mac run is a `--platform mac` build.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_used 0x6800000` | [`game.tsv`](game.tsv) | the modules are 100 MB, more than the 64 MB the host reports as used by default; `main` sizes its heap as the pool minus the used memory, so with the default the request failed, the game got a 0.03 GB heap and crashed on its first allocation. Modules + 4 MiB, as `profile_guess.mem_used`. The heap is then 3.11 GB |
| `shader_key_skip 0x770+0x78` | `game.tsv` | for two vertex shaders (`03749598c3c656f4`, `2b17c56bc893e2b0`) the engine hands the game the control section of an earlier program, which differs from the shipped one only in GLSLC's compile information (8 bytes at `+0x7d0`), so their keys missed the library and every 3D draw was dropped while the UI still drew. The range is Batman: Arkham Knight's. It makes the split share a key between shaders that differ only there: the cache's 3,393 shaders became 3,312 |
| `mem_reserve`, `mem_alias`, `mem_stack`, `app_pool`, `sys_resource` | `game.tsv` | kept as the provisional profile guessed them |
| `MultiViewport` for geometry shaders that write the viewport index | the translator, `SpirvGenerator.cs` | two geometry shaders failed `spirv-val` without it (the Vulkan path, which Peanuts Native uses); see the commit before this one |

No `patches.py`.

## Shaders

3,313 shaders (compute 7, domain 5, geometry 2, hull 5, pixel 1,682, vertex 1,612), all built for the Mac.

- **The shaders are in `resources/nvn_shader_cache`**, a 30.5 MB container (magic `NVNC`, version 1, 3,225 records of 64 bytes, then the entries back to back) of whole GLSLC outputs; the entries parse with `shader_split.glslc_shaders`. No reader splits it: the script sees six GLSL sources and `glslcCompile` in `main` and expects run-time capture only, so a build from the NSPs stops at the capture step with `captured only 6 run-time shaders, the profile needs 2818`. For this profile the container was cut by hand into `glslc_capture/<FNV-1a of the output>.glslc` files beside the RomFS, which is where the split reads them, and the tables were split from those. A build from a dump needs that step, or a reader.
- The six GLSL sources in `main` are compiled at run time: a Mac boot of 30 s captures six outputs into the same folder (the capture step reuses it when it already holds every file the shader map names).

## Frame rate

The game presents at 30 fps. On the Mac the frame time is 33.3 ms (p50) with GPU 15 ms (p50).

## Open

- The intro movie once drew as vertical stripes on the Android handheld; not reproduced or investigated.
- Not tested: gameplay beyond the opening room, controllers other than the keyboard on the Mac, an iPhone or iPad.
