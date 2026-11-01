# Digimon Story Cyber Sleuth: Complete Edition (`010014E00DB56000`)

Update 1.0.3 (v196608), SDK 6.4.0. Media.Vision's MVGL engine (the PC version's), rendering with **OpenGL**, not NVN: NVIDIA's desktop GL 4.5 core profile through EGL, which the host implements on Metal (`dm/gfx/gl/hle_gl.m`). CRI middleware plays the sound (one 6-channel AudioOut) and the USM movies. Modules `main`, `subsdk1` (the SDK's movie library) and `sdk`; `subsdk0` is NVIDIA's OpenGL driver and is never loaded. Plays on the iPhone.

On the Mac: the logos, the title, the privacy policy, the DigiLab story choice, the EDEN chat opening (movies and real-time scenes), character creation and the login, into EDEN (the fly-in, then its entrance plaza in 3D with Taiga under control), with sound, at 60 fps in the menus and 30 from the story on (the game's own rates); no red line. About 0.95 GB resident (0.3 GB of Metal allocations in EDEN), flat over half an hour.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk1 sdk` | [`game.tsv`](game.tsv) | load order as on Horizon (`subsdk1` = NintendoSDK_movie). `subsdk0` is NVIDIA's OpenGL driver, which programs the GPU through NvRm: never loaded, the GL layer answers `main`'s EGL imports |
| `sys_resource 0x0` | `game.tsv` | no system resource, so no virtual address memory: `nninitStartup` gives its malloc a heap of the pool − used memory (3.13 GB of the 3.25 GB heap region), which it maps only as it touches it |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the 4 GB layout of the games without virtual address memory |
| `mem_used 0x4a00000` (74 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory |
| No NVN shader tables | `game.tsv` `shader_note` | no NVN shaders: `gl_shaders.py` writes the GL library and its table, not the profile |
| The GL path | `main`'s imports | `profile_guess.py` gives `graphics gl` when `main` imports `eglCreateWindowSurface` and names `glShaderSource`; of the 22 games extracted ([rendering.md](../../../docs/reference/rendering.md#telling-a-gl-game-from-an-nvn-or-vulkan-one) says), only Digimon does |
| glslang | packing | for a GL game the NSP script runs `gl_shaders.py` in place of the NVN shader split and chain build (Mac and iOS libraries in `<id>_build/shader/gl` and `shader/ios/gl`); it needs `brew install glslang` |

No `pad_layout`: the virtual gamepad starts in the 2D layout (the default).

## Rendering (OpenGL)

- **Telling the API apart:** `main` imports `nvnBootstrapLoader` and 13 EGL functions. Its graphics setup (`main+0x5ed8f0`) makes an NVN device first, which only a helper beside the movie player uses (`main+0x528000`–`+0x52c400`: a memory pool and four textures next to the movie's GL textures; 30 NVN functions, all in that helper). Then `eglGetDisplay`, `eglBindAPI(EGL_OPENGL_API)`, a 4.5 core context, a window surface on `vi::CreateLayer(w, h)` and a glad loader (`main+0x52c460`). Absolum imports the same EGL functions (SDL3) but renders with NVN and names no GL function.
- **Which GL functions:** glad asks `eglGetProcAddress` for 2,956 names and stores each in a global its code reaches through the GOT. A census of the GOT slots the code loads outside the loader (`dm/tools/loader_census.py`) gives the 79 functions the game calls: buffers, vertex arrays, textures (2D and cube, S3TC), framebuffers and renderbuffers, programs with plain `glUniform*`, `glDrawElements` / `glDrawArrays`, and three compatibility calls (`glTexEnvi`, `glColorTable`, `glPixelTransferi`). No uniform buffers, instancing or buffer mapping. Every other name gets a null pointer.
- **Current: the GL layer.** One context; GL calls are encoded into Metal as they come, one command buffer per frame; GL's bottom-up orientation through the NVN layer's lower-left origin (`mtl_set_origin_lower_left`: flipped viewports, reversed winding, a flipped present); textures made on first use from the levels given (the loader defines them level by level); buffers orphaned on `glBufferData` (a new `MTLBuffer` when the GPU may still read the old one); the window surface (the size of the game's `vi::CreateLayer`, 1920x1080) presented through the NVN layer's present path, paced by the swap interval.
- Without `glBindAttribLocation` the game's attribute index is the Metal one: `glGetAttribLocation` returns the shader's own attribute numbers.
- **Resolved:** Metal refuses a swizzle on a texture that can be a render target (`Texture swizzling is not compatable with MTLTextureUsageRenderTarget`, in the first story scene), so only luminance / alpha formats get a swizzle; Metal reads a channel a format lacks as 0 (alpha 1), as GL does.
- **Present:** three window textures through the NVN layer's present path (`mtl_ext_tex`). The game sets the swap interval to 60 ÷ its frame rate before each swap: 1 at the title and in the DigiLab, 2 from the story's opening on.

## Shaders

- **Current:** GLSL ES 3.20 source compiled at run time with `glShaderSource` / `glCompileShader`: 1,682 `.shad` files in the MVGL archives (615 distinct) and 37 strings in `main` (post effects, FXAA, shadows, the movie's YUV).
- `dm/gfx/tools/gl_shaders.py <RomFS> <main> <dir> [--ios <dir>]` translates them offline with glslang and SPIRV-Cross into `<dir>/gl/` (676 shaders). A string in `main` says nothing of its stage, so it is translated as each stage that compiles (one fragment shader defines `vPosition` as `gl_Position`). The host finds each shader by the hash of its text and its stage.
- What NVIDIA's compiler accepts and glslang does not, fixed up on the preprocessed text: the GLSL ES 1.0 functions `textureCube` / `texture2D` in ES 3.20 code (89 fragment shaders; with `-V` glslang reads `textureCube` as Vulkan's texture type, so a macro cannot rename it), `out vec4 gl_Position` redeclared through a macro (the built-in sprite shader), and `textureOffset` with an offset that is not a constant (FXAA), which becomes a plain sample at P + offset ÷ level 0 size.
- `DM_GL_CAPTURE=<dir>` writes every source the game compiles, which `gl_shaders.py` reads like a RomFS; every one captured on the route so far is in the archives or `main`.

## Files

- Everything but the movies is in nine MVGL archives (`DSDB*.nx64.mvgl`: `MDB1`, a name tree, doboz-compressed entries; `dm/gfx/tools/mvgl.py` lists and reads them); four of them are CRI AFS2 sound banks under the same extension. The profile's `romfs_marker` is `DSDB.nx64.mvgl`.

## Movies

- The USM movies are loose CRI files. Their H.264 is decoded through the SDK's movie library (`subsdk1`, the host's decoder) into NV12 buffers (3,110,400 bytes a 1920x1080 frame); the game uploads the planes into a `GL_RED` and a `GL_RG` texture (two `glTexSubImage2D` a frame) and converts YUV in its own shader (in `main`, `SamplerY` / `SamplerUV`).

## Services

SDK 6.4.0 boots, one each:

- **Resolved: `UNIMPL eglGetDisplay`, then `Break` right after `nv::InitializeGraphics` and `vi::OpenDefaultDisplay`** (`main+0x5ed9d0`): the game renders with GL through EGL. The host's GL layer answers the EGL imports and `eglGetProcAddress`.
- **Resolved: SIGBUS in `__vfprintf` under `aoc_load`, from `h_aoc_Count`** (a store to `0x…67ff0`, right below a page of the guest thread's stack): the first add-on content query lists `DM_AOC_DIR` and logs, a few KB of stack, and the game asks from a thread with little left. `aoc::CountAddOnContent` and `ListAddOnContent` now run on the big host stack (`HLE_FORCE_BIG`, `hle_misc.c`).
- `vi::CreateLayer(w, h)` keeps the layer's size for the window surface.

## Mac routes

- A fresh save reaches EDEN with `DM_KBD=Taiga DM_KEYS="f2000:A*26/200,f7300:Right,f7400:A,f7600:A,f7800:Right,f7850:A,f8100:A*39/250,f9000:Plus*15/600,f18000:Down*3/1200,f18100:A*3/1200,f21700:Left*300/250,f21800:A*300/250"` (the title, the privacy policy, the DigiLab: Cyber Sleuth, no options, A through the opening chat; the login form comes at frame 18,500 to 19,700).
- The name is a field of that form: A opens the keyboard, and the game's name screen stays up about 500 frames after the text arrives, where another A opens it again, so Down and A come 1,200 frames apart. The login prompt defaults to No, hence Left before each A from there; EDEN at about frame 22,500.

## Found on this game

General host changes first needed here; each applies to every game:

- **The OpenGL layer** (`dm/gfx/gl/hle_gl.m`: EGL, GL on Metal, `mtl_set_origin_lower_left`, `mtl_ext_tex` presents) and **offline GLSL translation** (`dm/gfx/tools/gl_shaders.py`: glslang `-V -R`, SPIRV-Cross, varyings matched by name, `--fixup-clipspace`; `DM_GL_CAPTURE`): [Games that render with desktop OpenGL through EGL](../../../docs/reference/rendering.md#games-that-render-with-desktop-opengl-through-egl), [GLSL source translated offline and found by hash](../../../docs/reference/rendering.md#glsl-source-translated-offline-and-found-by-hash).
- **MVGL archives** (`dm/gfx/tools/mvgl.py`).
- **Loader census** (`dm/tools/loader_census.py`) and the `graphics gl` guess in `profile_guess.py`: [02, NVN surface](../../../docs/agents/02-survey.md#nvn-surface).
- **Add-on content queries on the big host stack** (`HLE_FORCE_BIG`): [Add-on content queries on a small guest stack](../../../docs/reference/services.md#add-on-content-queries-on-a-small-guest-stack).

## Open

- The player-facing [README](../../../README.md) lists the games that start in the 2D layout and says the others start in 3D; Digimon is not in that list, but its profile has no `pad_layout` (the default, 2D).
