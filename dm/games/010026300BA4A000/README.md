# Slay the Spire (`010026300BA4A000`)

Version 1.0 (base game), SDK **7.3.2** (`NintendoSdk_nnSdk-7_3_2`, `NintendoSDK_NVN-7_3_2`, `NintendoSDK_OpenGL-7_3_2`). Mega Crit's **libGDX** (Java) game on **MonoGame.Switch**, compiled ahead of time to native code by **BRUTE**: `main` holds the source paths `MonoGame.Switch/Brute/mono/mscorlib/…` and the class `BRUTE.Runtime`, and its managed strings (`com.badlogic.gdx…`, `com.megacrit…`) are UTF-16. The RomFS holds the game's pictures (PNG, JPEG, libGDX `.atlas`), Ogg Vorbis sound, JSON localization and its GLSL shader files.

It renders with **OpenGL**, not NVN: NVIDIA's desktop GL through EGL, which the host implements on Metal (`dm/gfx/gl/hle_gl.m`). MonoGame's GL loader takes every GL function from `eglGetProcAddress`. Modules `main` and `sdk`; `subsdk0` (NVIDIA's GL driver) is never loaded.

On the Mac: several full runs played with sound, in a window and fullscreen, from a save carried over from another device, with no bug seen. A per-frame probe in fullscreen (the `eglSwapBuffers` returns against the frames' presented times) measured 60 fps on screen at a fixed 60 Hz and under ProMotion: 2,184 of 2,190 frames at 16.7 ms.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | `main` takes nothing from NVIDIA's GL driver: the GL layer answers its EGL imports, so `subsdk0` stays unloaded |
| `sys_resource 0x0`; `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `mem_used 0x7600000` | [`game.tsv`](game.tsv) | the NPDM declares no system resource, so `main` sets a heap of at most the pool minus 64 MB, mapped as it is touched |
| `shader_groups /=romfs` | [`game.tsv`](game.tsv) | no NVN shaders: the profile's NVN tables stay empty and the GL library comes from `gl_shaders.py`, which reads the RomFS's `.vs` / `.fs` files |

No rewriter patches, no capture table and no host behaviour of its own: everything it needed was general (below).

## Rendering (OpenGL)

- **Detected as OpenGL from UTF-16 names:** `main` names `glShaderSource` once, as a managed (UTF-16) string and never as C text: [GL function names as UTF-16 strings](../../../docs/reference/rendering.md#gl-function-names-as-utf-16-strings).
- **`glGetActiveAttrib`:** libGDX's `ShaderProgram` lists each program's attributes after linking; without it MonoGame's null delegate threw `NullReferenceException`: [`glGetActiveAttrib`, and a loader that reuses one name buffer](../../../docs/reference/rendering.md#glgetactiveattrib-and-a-loader-that-reuses-one-name-buffer).
- **A fixed 1280x720 picture:** the game draws 1280x720, presented scaled to the window. A larger EGL surface, tried during bring-up, got the same 1280x720 in its lower-left corner, so the game does not size its targets by the surface.

## Shaders

- **Current:** 11 distinct GLSL ES 1.00 shaders (4 vertex, 7 fragment) from the 14 files in the RomFS's `shaders/` folder (`spritebatch.vs` / `.fs`, and `blur`, `grayscale`, `outline`, `redSilhouette`, `water`, `whiteSilhouette`, each a `.vs` and a `.fs`), translated by `gl_shaders.py`. A capture run with an empty library found the game compiles exactly these: [libGDX's `.vs` / `.fs` shader files](../../../docs/reference/rendering.md#libgdxs-vs--fs-shader-files).

## Found on this game

General host and tool changes first needed here; each applies to every game on the same path:

- **Resolved: an OpenGL game taken for NVN**, its GL names being UTF-16: [GL function names as UTF-16 strings](../../../docs/reference/rendering.md#gl-function-names-as-utf-16-strings).
- **Resolved: `glGetActiveAttrib` and `GL_ACTIVE_ATTRIBUTE_MAX_LENGTH`**, and `DM_GL_MISSES` / `DM_GL_STUBS` with a loader that reuses one name buffer: [`glGetActiveAttrib`, and a loader that reuses one name buffer](../../../docs/reference/rendering.md#glgetactiveattrib-and-a-loader-that-reuses-one-name-buffer).
- **Resolved: libGDX's `.vs` / `.fs` shader files**, read by `gl_shaders.py` and counted by the survey and the build: [libGDX's `.vs` / `.fs` shader files](../../../docs/reference/rendering.md#libgdxs-vs--fs-shader-files).

## Open

- **Not tested on the iPhone.**
- **Native resolution:** the game's 1280x720 is scaled to the window; drawing at the window's size would need the game to size its own targets by the surface, which it does not.
- **A report of stutter at 120 Hz**, once, in fullscreen: not reproduced (the probe above found clean 60 fps at 60 Hz and under ProMotion, and later sessions were fine). If it recurs, the log right after it is the next step.
