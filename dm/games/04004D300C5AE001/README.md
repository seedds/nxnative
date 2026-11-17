# Kirby and the Forgotten Land – Nintendo Switch 2 Edition + Star-Crossed World (`04004D300C5AE001`)

Nintendo Switch 2 Edition, **Program 1** (`04004D300C5AE001`) of the application `01004D300C5AE000`, update v65536 (1.1.0), SDK 19.3.7 built for the Switch 2 (Ounce), NVN2 (Ampere shaders). HAL Laboratory's engine on NintendoWare, as the [Switch 1 game](../01004D300C5AE000/README.md), which has its own executable and shaders and its own profile. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays on the Mac at 60 fps.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000`, `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM declares 16 MB of system resource (virtual address memory) and `main` never sets a heap: the game maps its memory in the 4.375 GB alias region itself; the pool is the Switch 2's 9 GB, as the `sdk` is built for it |
| `modules main subsdk0 sdk` | `game.tsv` | every module `main` imports from; `subsdk0` is the movie library |
| `shader_groups` (15 RomFS folders) | `game.tsv` | 93,348 BNSH / BAHS shaders in the RomFS and `main`; see [Files and shaders](#files-and-shaders) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 at boot (the NSP script's address-region check), so the pipeline bases made before it runs are made for that mode |
| `nn::pl::GetBuiltInFontDataSet` takeover | `dm/host/hle_sdk16.c`, [PR #2548](https://github.com/seedds/nxnative/pull/2548) | the game asks for its fonts with it at boot; unhooked, the SDK aborts on `pl:u` ([Built-in fonts and the audio device notification of SDK 21](../../../docs/reference/services.md#built-in-fonts-and-the-audio-device-notification-of-sdk-21)) |

No code of the game is cut, and no run-time key is set.

## Input and dependencies

- The input is a raw dump with the Edition's program 1 (`program/1`: its ExeFS, RomFS and control data), which main does not pack yet. [PR #1639](https://github.com/seedds/nxnative/pull/1639) packs and runs a raw dump's program 1 (made for Super Mario Bros. Wonder's Switch 2 Edition).
- The build tested also carried two fixes to #1639's branch against current main: `nn::pl::RequestLoadNxFont` taken over once (main's takeover and the branch's both registered it, and the loader aborts on a duplicate), and the damaged-file check reading a raw dump's RomFS folder, which has no `RomFS.img`.
- [PR #2548](https://github.com/seedds/nxnative/pull/2548): the font takeover above.

## Files and shaders

- `shader_map.tsv`: 93,348 shaders from the RomFS and `main`; `shader_facts.tsv`, `shader_pairs.tsv` and `pso_pairs.tsv` from the same split.
- 93,343 translate; 5 do not: their Ampere instruction 0x80d at offset 0x140 has a form the translator only knows as the guard band cull (`000fc80003fe0000c00432110000780d`).
- `pso_manifest.tsv`: the prewarm seed list, 9,202 shaders, 1,712 render pipelines and 16 compute pipelines recorded on the Mac in the play below, every shader key in the shader map.

## Testing

- 2026-11-12, Mac (MacBook Pro M4 Pro, macOS 27.2.1), a build of #1639's branch with the fixes above and #2548: about 9.5 minutes of play from boot (34,200 frames) at 60 fps, frame time p50 16.66 / p95 16.85 ms, CPU 4.78 W; no crash or abort.

## Open

- Two shaders fail to load at run time (`0359f398f2a318cf`, `1b5c7942ff49cc89`: no library); whether they are among the 5 untranslated above is not checked.
- `[RED] texture type mismatch` lines: a fragment shader declares slot 1 or 2 as a 2D texture and the game binds a 2D array there (about 30 lines in the run); bound as is, the picture not checked against the console.
- 173 `[RED] more than 16 font engines with a small work buffer` lines in the run.
- The rest of the game, iOS, and the Star-Crossed World content are untested.

## Found on this game

- [Built-in fonts and the audio device notification of SDK 21](../../../docs/reference/services.md#built-in-fonts-and-the-audio-device-notification-of-sdk-21): `GetBuiltInFontDataSet` at boot.
