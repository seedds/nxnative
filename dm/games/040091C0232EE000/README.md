# Persona 3 Reload (`040091C0232EE000`)

A Switch 2 game: the base game with update v262144, SDK 19.3.5 built for the Switch 2 (`Ounce`). ATLUS's game on Unreal Engine 4.27.2 (`SDK MW+EpicGames+UnrealEngine-4_27_2`, project `xrd777`) with IoStore containers, NVN2 and CRIWARE. Modules `main`, `subsdk0` (the movie module) and `sdk`. Packed from a NUP container: the base package holds only the code, the update the code and the RomFS (21.5 GB), and the 14 DLC records are licences with no files (their content is in the game's own data).

Status: plays on the Mac. A run in a window on 2026-11-14 went past the network-preferences dialog and the main menu into gameplay, which played with no issue seen. No IPA has been built and nothing has run on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game sets near = 0 at startup; the NSP script's address region check found it |
| `tex_dyn_slots on` | `game.tsv` | compute shaders that pick images from c2's table at run time: without it 150 of them failed reflection conversion |
| FMallocBinned3 constructor call cut, engine region given what the alias region has room for | the rewriter's engine-wide rules (`dm/tools/ue_malloc.py`, `dm/tools/ue_region.py`), no build ID | as the other Unreal 4.27 games |
| 64 GB alias region: `mem_reserve 0x1028000000`, `mem_alias 0x1000000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | the provisional profile's layout for the Mac; not fitted for an iPhone |
| `sys_resource 0x5000000` | `game.tsv` | the NPDM declares 80 MB of system resource |
| `shader_groups GlobalShaderCache=global #Engine/=engine #xrd777/=game` | `game.tsv` | the global shader cache, the engine's materials and the game's |

## Shaders

- [`shader_map.tsv`](shader_map.tsv): each material's shaders are inline in its cooked package, the packages are in the IoStore containers (`pakchunk*-Sage.utoc`, encrypted, key from `main`), and the global shaders are in `pakchunk0-Sage.pak`'s `Engine/GlobalShaderCache-SF_GLSL_SAGE.bin`. `pakchunk1`, `2` and `4` hold audio and fonts only.
- UE 4.27's `SF_GLSL_SAGE` records are LSLG records (as on the Switch) with an NVN2 control section (`uepak.py` `lslg`). The global shader cache also holds ray tracing records (stage tags `S1` to `S3`, `FDefaultMainCHS`), which are left out.
- Open, from the build of 2026-11-14 (31,805 of 32,285 shaders translated):
  - 449 shaders use image atomics on an image picked at run time, which the translator refuses (`Metal image atomics require a statically resolved image handle`).
  - About 30 shaders stop at Ampere instruction `0xf9b`, which is not implemented.
  - Compute shaders that write a 1D buffer image picked at run time (`cp_ib_cb0_40000000_`, 59 shaders) are bound to the dummy image, so those writes are lost, and 9 such accesses in 5 shaders are deleted by the translator.
  - One shader was not in the shader map in the run. [`pso_manifest.tsv`](pso_manifest.tsv) is empty: no prewarm list has been recorded yet.

## Add-on content

The 14 DLC records become empty images (`aoc/<index>.img`), so the host reports them as owned (`hle_misc.c`: `aoc: reporting 14 add-on contents`), as for Fire Emblem Engage.
