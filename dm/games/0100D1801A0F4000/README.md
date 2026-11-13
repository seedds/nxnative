# Risk of Rain Returns (`0100D1801A0F4000`)

Versions 1.0.5 (update `v196608`) and 1.0.0 (base `v0`), SDK **15.3.2** (`nnSdk-15_3_2`, `NintendoSDK_NVN-15_3_2`, `NintendoSDK_OpenGL-15_3_2`, `NintendoSDK_movie-15_3_2`). A **GameMaker Studio 2** game: the RomFS holds the runner's data file (`game.win`, `FORM`/`GEN8`), cutscenes (`data/cutscene/*.mp4`), fonts (`data/font`), trials, stages, and localization under `language/`.

It renders with **OpenGL**, not NVN: `main` imports EGL entry points and takes every GL function through `eglGetProcAddress`. The context is `OpenGL 3.2`. Modules `main`, `subsdk1` (`NintendoSDK_movie`) and `sdk`; `subsdk0` (NVIDIA's GL driver) is never loaded.

On the Mac: plays past `nnMain`, mounts save and RomFS, plays cutscenes with VideoToolbox hardware decoding, through title and menus at solid 60 fps with sound.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk1 sdk` | [`game.tsv`](game.tsv) | `main` takes nothing from NVIDIA's GL driver: the GL layer answers its EGL imports, so `subsdk0` stays unloaded |
| `sys_resource 0x0`; `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `mem_used 0x4800000` | [`game.tsv`](game.tsv) | the NPDM declares no system resource, so `nninitStartup` gives its malloc a heap of the pool minus used memory (3.25 GB), mapped as it is touched |
| Refused matchmaking NRO | [`services.md`](../../../docs/reference/services.md#online-sdks-in-nros-are-never-loaded) | `nro/RORRNXExt.nro` exports `nx_matchmaking_session_set_description`; refused as an online SDK so the game plays offline |
| [`gl_capture.tsv`](gl_capture.tsv) | profile | the 84 shaders the runner puts together at run time, which no game file holds; the NSP script captures them on the Mac; see [Shaders](#shaders) |
| `shader_groups /=romfs` | [`game.tsv`](game.tsv) | no NVN shaders: the profile's NVN tables stay empty and the GL library comes from `gl_shaders.py` |

## Rendering (OpenGL)

- **Shaders at run time:** the runner writes each variant of its GLSL as text, in **GLSL ES 1.00 without a `#version` line** (`attribute`, `varying`, `gl_FragColor`, `precision mediump`). Captured on the Mac and translated to Metal by `gl_shaders.py`: [GLSL ES 1.00 sources without a `#version` line](../../../docs/reference/shaders.md#glsl-es-100-sources-without-a-version-line).
- **Draws from client memory:** rendered via client-memory vertex arrays and standard GL draw paths.
- **Movies:** opening and outro cutscenes (`intro_desktop.mp4`, `outro.mp4`) decode through VideoToolbox in `subsdk1` (`NintendoSDK_movie`).

## Shaders

- **Current:** 84 sources in [`gl_capture.tsv`](gl_capture.tsv) — the runner's sprite, UI, effect, and composite programs — captured on the Mac with `DM_GL_CAPTURE` and translated to one `.metallib` by `gl_shaders.py`.

## Audio

- Background music and sound effects (Ogg Vorbis tracks in the RomFS) play without underruns.

## Found on this game

General host changes first needed here:

- **Resolved: Online SDK refusal for Switch Matchmaking extension** (`RORRNXExt.nro`): [Online SDKs in NROs are never loaded](../../../docs/reference/services.md#online-sdks-in-nros-are-never-loaded).
- **Resolved: EGL window surfaces and render targets zero-initialized for Metal**: [EGL window surfaces and render targets zero-initialized for Metal](../../../docs/reference/rendering.md#egl-window-surfaces-and-render-targets-zero-initialized-for-metal).

## Open

- **Online multiplayer:** the game uses `RORRNXExt.nro` on Switch for matchmaking and network sessions, which the host refuses; online matchmaking is unserved, local play only.
- **Extended gameplay validation:** tested through cutscenes, menus, audio playback, and gameplay frames on Mac. Full run completion remains to be verified.
