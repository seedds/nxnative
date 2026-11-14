# Unrailed 2: Back on Track (01006D00241E6000)

Provisional bring-up on one Mac (Apple M3 Max, macOS 26.6.2, Xcode 27.2), update v327680 (v925.eab7b7d5).
It reaches the title screen in a window with sound. Nothing beyond that was checked. The profile is the survey's provisional one with four memory values changed.

## Engine
- Godot 4.4.1 (W4 Games console build, mono), NintendoSDK 20.5.28, NVN "Forward Mobile".
- The RomFS is a Godot PCK (`GDPC`, format 2) with 1,138 `.godot/w4_shaders/*.nvn.w4_shader` files.
- `main` imports `glslcCompile` (subsdk0) and holds 93 GLSL sources, so the build asks for a run-time capture. The game does not compile at boot: it ships its shaders. The capture boot aborts after about 3 s because no shader map exists yet ("captured only 0 run-time shaders").

## Shader format (observed)
- A `.w4_shader` file starts with `GRSC`, u32 version 2, `NVNC`, then a small table with the shader name, then concatenated zstd frames.
- Each frame decompresses to one complete GLSLC output (magic 0x19866891, whole length at +0x44).
- 39,068 frames in 1,138 files, 2,884 unique.
- PCK directory entry: u32 path length, path, u64 offset, u64 size, 16-byte MD5, u32 flags. The data base is the position of `GDPC` plus the u64 `file_base` at header offset 24 (confirmed by the first file's MD5).

## How to build
1. Run the NSP script once. It extracts the game and stops at the GLSLC capture.
2. `python3 dm/tools/w4_to_glslc.py <the _build folder> v327680` writes the unique outputs to `extract/v327680/glslc_capture/`.
3. Run the NSP script again. The capture step says "reused", the split finds 2,884 shaders and all 2,884 Mac shaders compile. No engine patch is needed.

`shader_split.py` does not read `.w4_shader` files itself. A reader there would make step 2 unnecessary.

## Memory values that worked
| key | survey value | working value | what happened with the survey value |
|---|---|---|---|
| `mem_stack` | `0x10000000` | `0x100000000` | thread creation aborted after 512 QueryMemory calls |
| `mem_reserve` | `0x140000000` | `0x230000000` | grows with `mem_stack` |
| `mem_alias` | `0x118000000` | `0x118000000` | unchanged |
| `app_pool` | `0xCD500000` | `0x180000000` | the windowed run read a null pointer after `MemoryPoolInitialize: non-virtual pool has no storage flags=0x22 size=0x8000000` |

The stack and pool values are working guesses, larger than needed and not tuned.

## Open points
- `[RED] texture format 0x22 has no Metal equivalent` is logged once; its visual effect is unchecked.
- No per-game patches, no tuned memory layout, no frame step; controls, saves and long sessions are untested.
- A detector fix would help games like this one, with GLSL text in `main` but shipped shaders.
