# DOOM 3 (`010029D00E740000`)

Update 1.0.5 (v327680), SDK 14.3.0. id Software's id Tech 4 with the BFG Edition renderer: DOOM 3, Resurrection of Evil and The Lost Mission, its data in id's `.resources` containers, its movies Bink, its sound FMOD. It renders with **OpenGL**, not NVN: NVIDIA's desktop GL 4.5 through EGL, which the host implements on Metal ([OpenGL layer](../../../docs/reference/rendering.md#opengl-layer)); `subsdk0` (NVIDIA's GL driver) is not loaded.

On the Mac (M6, macOS 27.2): the title, the campaign menus, a new Doom 3 game on Marine through the intro movie into Mars City Hangar, its opening cutscene and play there (walking, turning, saving and loading), lit, with stencil shadows and the HUD, at 60 fps (300 presents per 300 display ticks), about 800 draws a frame; about 0.9 GB footprint and 0.35 GB of Metal allocations. Played for about 8 minutes in all, in the hangar only: no combat, flashlight or later level yet. Not run on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| [`gl_capture.tsv`](gl_capture.tsv) | profile | the 66 distinct shaders its renderer makes from its 82 render programs, which no game file holds; the NSP script's capture run has the game convert them all on the Mac; see [Shaders](#shaders) |
| `modules main sdk` | `game.tsv` | `subsdk0` is NVIDIA's OpenGL driver, which programs the GPU through NvRm: never loaded, the GL layer answers `main`'s EGL and GL calls |
| `sys_resource 0x0`; `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource, so no virtual address memory; `main` sets a heap of at most the pool − 64 MB (3,328 MB) |
| No NVN shader tables | `game.tsv` `shader_note` | no NVN shaders: `gl_shaders.py` writes the GL library and its table, not the profile |

No rewriter cut, build-keyed host row or profile switch: everything below is the GL layer, general, and the render program capture, bound to the engine (found from its code).

## Rendering (OpenGL)

- **Resolved: an abort 2 s after `EGL context: OpenGL 4.5`.** The renderer checks a fixed list of extensions and stops with "<name> not available" (`main+0x26220` in the abort's backtrace); it then asks for many functions only by their ARB names. [Extensions a GL renderer checks for, and its ARB names](../../../docs/reference/rendering.md#extensions-a-gl-renderer-checks-for-and-its-arb-names).
- **GL calls it makes** (with `DM_GL_STUBS=1` and `DM_GL_MISSES=1` on the way to Mars City): named uniform blocks (`glGetUniformBlockIndex`, `glUniformBlockBinding`, `glBindBufferRange` of a part of a buffer), two-sided stencil (`glStencilOpSeparate`, `glStencilFuncSeparate`), `glCopyTexImage2D` every frame, `glCompressedTexSubImage2DARB`, `GL_UNPACK_SWAP_BYTES`, `glIsSync`, `glShadeModel`, `glClientActiveTextureARB`, `glPolygonMode(GL_LINE)` once while it puts its state in order. It asks for its compatibility-profile calls (`glBegin`, `glVertex3f`, `glMatrixMode` …) and ARB assembly programs and does not call them here. Each is a general entry under [OpenGL layer](../../../docs/reference/rendering.md#opengl-layer).
- **Resolved: no shadows.** Its shadow volume programs `shadow` and `shadow_skinned` have no fragment shader ([GL programs with a vertex shader alone](../../../docs/reference/rendering.md#gl-programs-with-a-vertex-shader-alone)) and count front and back faces in one draw ([GL two-sided stencil](../../../docs/reference/rendering.md#gl-two-sided-stencil)).
- **Checked, not needed: invariant positions.** Its light passes draw where the depth equals the depth pre-pass; `gl_shaders.py` compiles with `-fpreserve-invariance` but adds no `invariant gl_Position`. Full-resolution shots of the hangar's floor, crates and a skinned marine show no speckling, so no invariance was added (issue #1180 reported misses "in places" without it on its build).
- **Resolved: `GL_TEXTURE_LOD_BIAS 0.5`** on some textures was dropped with a red line, from before Metal samplers had a bias; it is now the sampler's `lodBias`, so those textures sample half a level coarser, as on the Switch ([Smaller GL calls of an id Tech 4 renderer](../../../docs/reference/rendering.md#smaller-gl-calls-of-an-id-tech-4-renderer)).

## Shaders

- **Current:** the renderer converts each render program in `_common.resources` (`renderprogs/*.vertex` and `*.pixel`, HLSL-like source: 42 vertex and 40 pixel files) to GLSL 1.50 the first time it uses it, after a prefix `main` holds, and compiles it with `glShaderSource`. No game file holds the GLSL (`renderprogs/gl/` is the original game's ARB assembly, `renderprogs/cgb/` compiled PS3 code, and it writes no `renderprogs/glsl/` cache). The NSP script's capture run has the game convert every program itself, through its own `FindVertexShader` / `FindFragmentShader` (`main+0xa8fa8` / `+0xa9588` in 1.0.5, found from the code by `idtech4_programs.py`; the manager pointer at `main+0xea0978`): 82 conversions, 66 distinct texts (the skinned fragment programs share their source with the others, and `color` / `shadowDebug`, `bink` / `bloodorb_draw`, `gui` / `bink_gui` and others come out the same), all there at the first frame, about 1 s into a headless boot ([GLSL an id Tech 4 renderer makes on first use](../../../docs/reference/rendering.md#glsl-an-id-tech-4-renderer-makes-on-first-use)).
- **Resolved: the first capture played into the first level** (a route of A presses): 47 shaders by the title screen, 62 once Mars City Hangar had loaded, all of them byte for byte what the game's own conversion gives; it lacked `motionBlur` and `stereoInterlace`. The game's 256 materials name the material programs (heat haze, the blood orbs, the environment suit, `colorProcess`, `motionBlur`), all among the 82, and five names with no render program in the containers (`flare`, `pinch`, `portalsky`, `xray`, `tester`), which the renderer cannot load and nothing captures.
- **Resolved:** `main`'s two `#version 150` strings are that prefix, not shaders; the NSP script translated them, failed, and gave the game no capture ([GLSL prefixes in main are no shaders](../../../docs/reference/rendering.md#glsl-prefixes-in-main-are-no-shaders)).
- **Resolved:** its GLSL redeclares built-ins as plain varyings, declares the compatibility colour pair `gl_FrontColor` / `gl_Color` as varyings, and names a parameter `sampler` ([GLSL that NVIDIA's compiler accepts and glslang does not](../../../docs/reference/rendering.md#glsl-that-nvidias-compiler-accepts-and-glslang-does-not)); its skinned shaders read joints from a named block ([GL named uniform blocks](../../../docs/reference/rendering.md#gl-named-uniform-blocks)).

## Mac routes

Work folder `<NSP folder>/010029D00E740000-work`: `host.sh` builds the host from a worktree into `d3host` (`REPO=`), `boot.sh <run> [seconds] [VAR=value …]` boots with window and HUD (`SAVE=<dir>` copies a save in, `D3HOST=` another host; every shader it compiles goes to the build's capture folder), `ctl.sh <run> <line> …` drives it through `DM_CTL` (`shot <name>`, `wait <s>`).

- From a fresh save: A at the title (about 18 s), A Campaign, A New Game, A Doom 3, A Marine; A about 25 s later skips the intro movie; the hangar has loaded and its cutscene plays about 30 s after that. Plus is the pause menu (Save Game, Load Game).
- `save_ref` in the work folder: a save and the autosave in Mars City Hangar. Its campaign menu has Continue Game first; d-pad Up moves the cursor (the left stick does not), Load Game lists the autosave first.
- Play: the left stick moves, the right stick turns.

## Found on this game

General changes first needed here; each applies to every game:

- **Extensions a renderer checks for, ARB names, ARB assembly program entries**: [Extensions a GL renderer checks for, and its ARB names](../../../docs/reference/rendering.md#extensions-a-gl-renderer-checks-for-and-its-arb-names).
- **Named uniform blocks** in `gl_shaders.py`'s table and the GL layer: [GL named uniform blocks](../../../docs/reference/rendering.md#gl-named-uniform-blocks).
- **Two-sided stencil**: [GL two-sided stencil](../../../docs/reference/rendering.md#gl-two-sided-stencil).
- **Programs with a vertex shader alone**: [GL programs with a vertex shader alone](../../../docs/reference/rendering.md#gl-programs-with-a-vertex-shader-alone).
- **`glCopyTexImage2D` / `glCopyTexSubImage2D`**: [GL frame copies into a texture](../../../docs/reference/rendering.md#gl-frame-copies-into-a-texture).
- **`GL_UNPACK_SWAP_BYTES`**: [GL_UNPACK_SWAP_BYTES](../../../docs/reference/rendering.md#gl_unpack_swap_bytes).
- **`glCompressedTexSubImage2D`** on the compressed sub-image path: [Compressed OpenGL texture arrays](../../../docs/reference/rendering.md#compressed-opengl-texture-arrays).
- **Wireframe, sync queries and smaller calls**: [Smaller GL calls of an id Tech 4 renderer](../../../docs/reference/rendering.md#smaller-gl-calls-of-an-id-tech-4-renderer).
- **GLSL 1.50 fix-ups** (redeclared built-ins, the colour pair, `sampler`): [GLSL that NVIDIA's compiler accepts and glslang does not](../../../docs/reference/rendering.md#glsl-that-nvidias-compiler-accepts-and-glslang-does-not).
- **`main`'s GLSL prefixes are no shaders** (`profile_guess.py`, `gl_shaders.py`): [GLSL prefixes in main are no shaders](../../../docs/reference/rendering.md#glsl-prefixes-in-main-are-no-shaders).
- **The render program capture** (`idtech4_programs.py`, `DM_GL_RENDERPROGS`): [GLSL an id Tech 4 renderer makes on first use](../../../docs/reference/rendering.md#glsl-an-id-tech-4-renderer-makes-on-first-use).

Issue [#1180](https://github.com/seedds/nxnative/issues/1180) reported this game first, with a patch on an older `main`; its findings (the extension list, ARB names, the copy into `_currentRender`, two-sided stencil, vertex-only shadow programs, byte-swapped light images) were checked against the game here and implemented anew. Its input, iPhone A15 (ETC2 transcoding) and in-game settings parts are not in this port.

## Open

- Combat, the flashlight, later levels and Resurrection of Evil / The Lost Mission are not played yet (their shaders are captured).
- iPhone: not run. The GL layer is the same; the IPA needs an iPhone with BC formats (A17 Pro or later).
- Aiming is by the right stick only (the game has no mouse aim).
