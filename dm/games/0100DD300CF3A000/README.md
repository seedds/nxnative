# Timespinner (`0100DD300CF3A000`)

Base game 1.0.0 (v0), SDK 7.3.0. The game's C# runs as C++ compiled by Sickhead's BRUTE (its name is in `main`); its graphics are FNA's `OpenGLDevice` with MojoShader (`main`'s assertions name `D:/Projects/Switch/FNA/lib/MojoShader`), its sound XACT (`Content/Audio/*.xwb`, `.xsb`, `.xgs`). It renders with **OpenGL**, not NVN: NVIDIA's desktop GL through EGL, which the host implements on Metal ([Games that render with desktop OpenGL through EGL](../../../docs/reference/rendering.md#games-that-render-with-desktop-opengl-through-egl)).

On the Mac: boot, the title, a new game and play into the game, with sound and no red line but `eglBindAPI` (below). Not built for iOS.

## What it needs

| Setting | Where | Why |
|---|---|---|
| [`gl_capture.tsv`](gl_capture.tsv) | profile | the 14 shaders MojoShader writes at run time, which no game file holds; the NSP script captures them on the Mac; see [Shaders](#shaders) |
| `modules main sdk` | `game.tsv` | `subsdk0` / `subsdk1` are NVIDIA's OpenGL and Vulkan drivers, which program the GPU through NvRm: never loaded, the GL layer answers `main`'s EGL imports |
| `sys_resource 0x0`; `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource, so no virtual address memory: `main` sets a heap of at most the pool − 64 MB (a 3328 MB heap region) |
| `mem_used 0x6600000` | `game.tsv` | the rewritten modules take 102 MB, more than the default 64 MB |
| No NVN shader tables | `game.tsv` `shader_note` | no NVN shaders: `gl_shaders.py` writes the GL library and its table, not the profile |

## Shaders

- **Current:** the game's 12 effects (`Content/Effects/*.xnb`) are XNA effects, Direct3D 9 bytecode (`0xFEFF0901`) with one pixel shader each; MojoShader turns each into desktop GLSL 1.20 when the game loads it, as it does SpriteBatch's built-in effect (a vertex and a pixel shader). That makes 14 shaders, all in [`gl_capture.tsv`](gl_capture.tsv); the NSP script's capture boot found them all in 25 s.
- **Resolved:** `main` holds only the `#version 100` / `110` / `120` lines MojoShader puts before its GLSL, which the NSP script counted as shaders, so it never captured the real ones: [A version line with no entry point is not a shader](../../../docs/reference/rendering.md#a-version-line-with-no-entry-point-is-not-a-shader).
- **Resolved:** the GLSL is compatibility 1.20 (`gl_TexCoord[0]`, `gl_Color`, `gl_FrontColor`, `gl_FragColor`, all named through `#define`s): [Desktop GLSL 1.10 / 1.20 and its built-in varyings](../../../docs/reference/rendering.md#desktop-glsl-110--120-and-its-built-in-varyings).
- **Resolved:** a capture run (no library) stopped on MojoShader's `vs_flip_loc != -1` assertion after 13 shaders: [A program whose shader the library lacks still has its uniforms](../../../docs/reference/rendering.md#a-program-whose-shader-the-library-lacks-still-has-its-uniforms).

## Rendering (OpenGL)

- **Resolved:** the game stopped at start with `OpenGL ES 3.0 support is required! Entry Point: glBlendColor`: FNA's device checks 16 core functions the layer lacked ([GL functions FNA's OpenGL device requires](../../../docs/reference/rendering.md#gl-functions-fnas-opengl-device-requires)). It also asks for MojoShader's ARB and NV names, which it does not use with the glsl120 profile.
- **Resolved:** sprites and tiles of the scene were missing. The scene is a 512x256 target composed layer by layer (clear texture 26, draw the layer, attach texture 27 to the same framebuffer, draw 26 over it), and a clear waiting for its pass landed on texture 27 after each empty layer: [A waiting clear runs before an attachment changes](../../../docs/reference/rendering.md#a-waiting-clear-runs-before-an-attachment-changes).
- **Current:** the game binds the OpenGL ES API (`[RED] eglBindAPI(0x30a0): only desktop OpenGL (0x30a2) is implemented`) and gets the desktop context the layer gives; MojoShader then picks its glsl120 profile from the context's version string, and the game renders.

## Rewriter

- **Resolved:** the rewrite stopped with `AssertionError: jump table 2c0a220`: six switches in one function of `main` (`main+0x2c0a220` to `+0x2c0b2d8`, on a pixel's byte width) load their table entry through the base and add it through a copy: [The add reads a copy of the table base](../../../docs/reference/rewriter.md#the-add-reads-a-copy-of-the-table-base).

## Found on this game

General host and tool changes first needed here; each applies to every game:

- **The add reads a copy of the table base** (`patch_nso.py` `jump_offsets`): [rewriter.md](../../../docs/reference/rewriter.md#the-add-reads-a-copy-of-the-table-base).
- **A version line with no entry point is not a shader** (`profile_guess.py` `gl_sources`, `gl_shaders.py` `nso_strings`): [rendering.md](../../../docs/reference/rendering.md#a-version-line-with-no-entry-point-is-not-a-shader).
- **Desktop GLSL 1.10 / 1.20** (`gl_shaders.py` `legacy_glsl`, `legacy_io`): [rendering.md](../../../docs/reference/rendering.md#desktop-glsl-110--120-and-its-built-in-varyings).
- **Uniforms of a program whose shader the library lacks** (`hle_gl.m` `uni_from_source`): [rendering.md](../../../docs/reference/rendering.md#a-program-whose-shader-the-library-lacks-still-has-its-uniforms).
- **16 GL functions FNA requires, and copied stub names** (`hle_gl.m`): [rendering.md](../../../docs/reference/rendering.md#gl-functions-fnas-opengl-device-requires).
- **A waiting clear runs before an attachment changes** (`hle_gl.m` `fb_attach`): [rendering.md](../../../docs/reference/rendering.md#a-waiting-clear-runs-before-an-attachment-changes).

## Open

- iOS: not built or tested.
- Only the first scenes have been played; the occlusion queries, `glGetTexImage` and the per-face stencil the layer now has were not seen called.
