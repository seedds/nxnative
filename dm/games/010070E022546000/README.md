# Fate/hollow ataraxia REMASTERED (`010070E022546000`)

Switch update 1.0.2 (v131072, NACP `DisplayVersion` `"1.0.2 b723"`) over the base NSP (US and JP share `main`'s build ID `6c16aa06a25504cee9cf0f4f7b5e615c`), SDK 16.2.6, TYPE-MOON's Fuzz engine (`smash::fw`) rendering through Nintendo's OpenGL ES / EGL stack. Modules `main`, `subsdk3` (`NintendoSDK_movie`) and `sdk`; `subsdk0` (`NintendoSDK_OpenGL`), `subsdk1` (`GLSLC`) and `subsdk2` (`NintendoSDK_Vulkan`) are not loaded. All 78 runtime-built GLSL shaders (9 vertex, 69 fragment) are captured on the Mac (`gl_capture.tsv`) and translated offline by `gl_shaders.py`.

Plays on the Mac with no game-specific patches (`patches.py` not needed): 1920×1080 in docked mode (gamepad controls) or touch controls in handheld mode (`DM_OPMODE=0`). Not yet tested on iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout (`mem_reserve 0x100000000`, `app_pool 0xCD500000`, `sys_resource 0x0`) | [`game.tsv`](game.tsv) | NPDM declares 0 MB of system resource (no virtual address memory); `main` raises its heap inside the pool |
| Runtime GLSL capture list (78 shaders) | [`gl_capture.tsv`](gl_capture.tsv) | GLSL shaders are constructed at runtime and translated offline by `gl_shaders.py` |

## Found on this game

- [GL extension names of core functions](../../../docs/reference/rendering.md#gl-extension-names-of-core-functions) (`EXT_framebuffer_object` aliases via `gl_aliases.h`, [#2818](https://github.com/seedds/nxnative/issues/2818))
- [EGL's current context, per thread](../../../docs/reference/rendering.md#egls-current-context-per-thread) (`eglGetCurrentDisplay` / `eglGetCurrentContext` / `eglGetCurrentSurface`, [#2818](https://github.com/seedds/nxnative/issues/2818))
- [`glGetShaderSource` returns the stored shader text](../../../docs/reference/rendering.md#glgetshadersource-returns-the-stored-shader-text) (the engine reads compiled shader source back after compiling)
- [`nn::oe::GetDisplayVersion` reads the exact NACP string](../../../docs/reference/services.md#nnoegetdisplayversion-reads-the-exact-nacp-string) (`control.nacp` holds `"1.0.2 b723"`; truncating it to `"1.0.2"` made `GameDataContext` store version `2` instead of `723`, failing `SaveloadManager::setHollowSlotInfo`'s `>= 438` check and hiding all Entrance save slots, [#2930](https://github.com/seedds/nxnative/issues/2930))
