# Slay the Spire (`010026300BA4A000`)

Switch base version 1.0 (`v0`), SDK 7.3.2. Mega Crit's libGDX game runs on MonoGame.Switch, compiled ahead of time by BRUTE. It uses NVIDIA's desktop OpenGL through EGL. The host answers EGL on Metal and loads `main` and `sdk`.

On an Apple M6 running macOS 27.2.1, the base build created a fresh save and started an Ironclad run through the Act I map. The [original bring-up](https://github.com/seedds/nxnative/pull/2322) reports full runs with audio in windowed and fullscreen modes using a transferred save. iPhone validation is open.

## What it needs

| Setting | Why |
|---|---|
| `modules main sdk` | The host answers the renderer's EGL and GL calls, so the profile can omit `subsdk0`, the shipped NintendoSDK OpenGL driver. |
| `sys_resource 0x0`, `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `mem_used 0x7600000` | The NPDM declares zero system resource. `main` maps its heap from the application pool as needed. |
| `shader_groups /=romfs` | The packer builds the GL library from the RomFS. The NVN tables are empty. |

## Found on this game

- **Resolved:** the packer took the game for NVN. [GL function names in managed strings](../../../docs/reference/rendering.md#gl-function-names-in-managed-strings).
- **Resolved:** the first linked program needed active-attribute enumeration. [GL active-attribute reflection](../../../docs/reference/rendering.md#gl-active-attribute-reflection).
- **Resolved:** the packer missed the loose shaders. [Standalone GLSL in `.vs` and `.fs` files](../../../docs/reference/rendering.md#standalone-glsl-in-vs-and-fs-files).
