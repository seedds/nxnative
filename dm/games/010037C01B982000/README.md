# Xuan Yuan Sword: Mists Beyond the Mountains (`010037C01B982000`)

4.0.6 (v65536), SDK 15. DOMO's in-house engine on a self-hosted SDL: `main` ships SDL-switch 2.24.0, whose opengles2 render driver builds the GL context, and compiles its GLSL through it at run time; the opening movie (and the video module's playback) draws with NVN, compiled from GLSLC run-time output. Modules `main`, `subsdk2` (SDL + the game's own `glslcCompile`), `subsdk3`, `subsdk4` (the video module, `nvnBootstrapLoader`) and `sdk` (its exports are `main`'s build, `8618895246…`, the same hash as `subsdk2`). Not checked on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| The GLES entry points (`eglBindAPI(0x30a0)` accepted, `OPENGL_ES_BIT` in `RENDERABLE_TYPE`/`CONFORMANT`, `OpenGL_ES` in `CLIENT_APIS`, the API named in `GL_VERSION`, a `glShaderBinary` entry point) | `dm/gfx/gl/hle_gl.m`, [seedds/nxnative#2619](https://github.com/seedds/nxnative/pull/2619) | SDL's opengles2 driver matches on those answers and loads `glShaderBinary` through `eglGetProcAddress` at renderer creation: without them `SDL_CreateRenderer` fails ("Couldn't find matching render driver") and the game crashes on the NULL renderer. The game compiles from source; `glShaderBinary` is never called |

The profile started from the NSP script's provisional profile; the memory, module and shader-split values are its survey's.

## Rendering

- The game's GLSL lives as plain source in `main` (it names `E:/Work/swd3/swd3_repack/SS_SDK/SDL-switch-2.24.0/src/render/opengles2/SDL_render_gles2.c` and compiles with `glShaderSource`/`glCompileShader`): the host captures the sources at run time (`DM_GL_CAPTURE`) and `gl_shaders.py` turns them into the Metal libraries. 8 shaders (4 vertex, 4 pixel) cover boot to the first scene.
- The movie path is NVN: `subsdk4` compiles two GLSLC binaries at run time (captured with `DM_GLSLC_CAPTURE`, split into `shader_map.tsv`). Without `shader_map.tsv` the first movie aborts the process (`sig=6` in the video module).

## Input, language

- The default pad layout works (SDL's GameController mapping in `subsdk2`).
- `DM_LANG=zh-Hans` selects the game's Chinese; English is the default (`Game Language:english` in the log).
