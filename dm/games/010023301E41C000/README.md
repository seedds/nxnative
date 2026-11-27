# Fate/stay night REMASTERED (`010023301E41C000`)

Update 1.4.1 (v589824), SDK 16.2.6. TYPE-MOON's Fuzz engine (`smash::fw`), the same one Fate/hollow ataraxia REMASTERED runs ([its note](../010070E022546000/README.md)): 29 of the 61 shader sources here are byte-identical to that profile's 78, `c5b7066d3afd5bfe` (the sixteen-sampler screen one) among them. It renders with **OpenGL**, not NVN: NVIDIA's desktop GL 3.1 through EGL, which the host implements on Metal (`dm/gfx/gl/hle_gl.m`). Modules `main`, `subsdk3` (Nintendo's movie module) and `sdk`; `subsdk0` (NVIDIA's OpenGL driver), `subsdk1` (GLSLC) and `subsdk2` (NVIDIA's Vulkan driver) are never loaded, since the GL layer answers `main`'s EGL imports.

On the Mac: the disclaimer, `TYPE-MOON PRESENTS`, the prologue's narration and its first scene (the bedroom: full-screen background, character, dialogue and voice), the in-game menu behind `Y` with its save screen, and the backlog behind `X`, at 60 fps (10,500 presents in 180 s, the display link at 16.6 ms a tick) with sound. A four-minute run reached frame 13,500 with no `[RED]` line and no crash; the memory line read 1.95 GB footprint / 260 MB of Metal allocations at frame 1,800 and 2.03 GB / 250 MB at 12,600, with the texture count still growing as scenes loaded.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk3 sdk` | [`game.tsv`](game.tsv) | `subsdk0` / `subsdk1` / `subsdk2` are NVIDIA's OpenGL and Vulkan drivers and the GLSLC module, which program the GPU through NvRm or compile GLSL: never loaded, the GL layer answers `main`'s EGL imports, and the game compiles its GLSL itself through `glShaderSource` |
| [`gl_capture.tsv`](gl_capture.tsv) | profile | the 61 shaders the game compiles at run time, which no game file holds; the NSP script captures them on the Mac; see [Shaders](#shaders) |
| `sys_resource 0x0`; `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource, so no virtual address memory: `nninitStartup` sets a heap of pool − used memory and maps it as it touches it; the heap check asked for 3.14 GB of the 3.25 GB region the 4 GB block leaves |
| No NVN shader tables | `game.tsv` `shader_note` | no NVN shaders: `gl_shaders.py` writes the GL library and its table, not the profile |

## Rendering (OpenGL)

- **Telling the API apart:** `main` imports the EGL functions and names `glShaderSource`, and draws only through it: it makes a GL 3.1 context with `eglBindAPI(EGL_OPENGL_API)`. `subsdk2` (NVIDIA's Vulkan driver) is not loaded and nothing calls `vkCreateInstance`; `main`'s one `nvn` import is not what it draws with either.
- **Fixed-function state, not fixed-function drawing:** it turns off `GL_LIGHTING`, `GL_LIGHT0`, `GL_ALPHA_TEST` and `GL_TEXTURE_GEN_S/T/R/Q` before its first draw, as a GL 1.x-era renderer puts its state in order. That was seven `[RED] glDisable(…) not implemented` lines; the alpha test and the lighting switch-off are handled by the host's fixed pipeline, and the five left (`0x4000`, `0xc60`–`0xc63`) are silent now, the layer ignoring the switch-off of the caps it implements nothing for ([Fixed-function caps a GL 1.x renderer turns off](../../../docs/reference/rendering.md#fixed-function-caps-a-gl-1x-renderer-turns-off)).
- **Sixteen samplers in one shader:** `c5b7066d3afd5bfe` binds 16 combined samplers, past Metal's direct sampler table, which the layer's argument-buffer path handles (`TABLE_ABI 2`; the same shader as Fate/hollow ataraxia REMASTERED's, [GL resources exceed the direct Metal sampler table](../../../docs/reference/rendering.md#gl-resources-exceed-the-direct-metal-sampler-table)). The translated table gives it `r:0:33`.
- **What it calls:** the GL functions Fate/hollow ataraxia REMASTERED needed are the ones here too, and the layer has them: `glValidateProgram` right after every `glLinkProgram`, `glLineWidth(1)`, `glClearTexImage` to clear a new texture, and the `EXT_framebuffer_object` names its loader asks for ([GL extension names of core functions](../../../docs/reference/rendering.md#gl-extension-names-of-core-functions)). No red line and no crash came from any of them.
- **What it draws:** 39 draws a frame in the dialogue scene (two passes: the scene, then the UI), 10 pipelines by the resume question and 19 by the prologue's first scene; the frame is the 1920x1080 window surface, presented through the layer's bilinear pass.

## Shaders

- **Current:** every shader is GLSL the game compiles at run time; no game file holds one, so the NSP script captures them on the Mac (`DM_GL_CAPTURE` at `<id>_build/extract/<version>/gl_capture` until every file in [`gl_capture.tsv`](gl_capture.tsv) is there) and `gl_shaders.py` translates them into `shader/gl/gl_shaders.metallib` and its table, which the host looks each shader up by (the hash of its text). 61 sources (8 vertex) translate with no failure.
- **The capture covers the prologue's first scene:** the game compiles the first programs while the disclaimer is up (2 pipelines), the resume question and `TYPE-MOON PRESENTS` bring it to 10, and the prologue's dialogue to 19. The NSP script's capture run compiles missing stages on the fly (`DM_GL_CAPTURE_COMPILER`) and collects all 61 keys unattended; a manual `DM_GL_CAPTURE` run without a library or compiler skips draws (`draw with no usable program`) while still capturing every source reached, including with the A presses below.
- **The capture table is what makes the capture whole (checked 2026-11-21):** without a `gl_capture.tsv` the NSP script stops the capture run after 20 s without a new source, and that is exactly what this game's slow start defeats — its first 21 sources land in the first 2 s (the logos), and the next one only when the prologue starts, at about frame 1300 (22 s). The build then translates 21 sources, and every program of the prologue is skipped for want of a shader: the picture is black after the logos, with `[RED] … shader <key> (…) is not in the library` in the log. With the table the run keeps going until all 61 are there (36 s on an M4).
- **The sibling's capture overlaps:** 29 of the 61 keys are also in `010070E022546000`'s `gl_capture.tsv` (78 keys) — the two remasters share much of the engine's shader set, and each has sources the other never compiles. A fix to one of the shared sources shows up as a changed key in both profiles.
- **Not yet covered:** the config screen, the save and load screens beyond the menu they sit in, and the effects of later chapters may compile sources the capture has not seen; a run that draws something new prints `[RED] … shader <key> (…) is not in the library` and writes it to the capture folder, so a capture run in those screens completes the table.

## Audio

Sound works without a change: the audio renderer runs (24,000 frames in 180 s, 19,075 of them not silent), and the log's `audren` lines show the SDK 16 layout found at `AudioRendererImpl+0x60`.

## Save and resume

Saving works: `Y`, `Save`, a slot and OK write a new slot (the screen says `New save data created.` with the scene's thumbnail), and the host commits the transaction into `DM_SAVE` (`save commit #1` … `#3`). All of the game's user data lives in `data/udat_rel.bin` (about 6 KB after one save) beside `data/system/version.conf`, and a run that ends while a scene is open also leaves the suspend data behind the boot question `Resume from suspended prologue scene?`, which takes A. Deleting the `DM_SAVE` folder is how a run starts from the disclaimer again.

## Mac routes

- Past the disclaimer and the resume question onto the prologue: `DM_KEYS=f800:A,f1100:A` (the second answers the question when the save folder has a suspend file).
- Advance the prologue: `DM_KEYS=f700:A*240/40` presses A every 40 frames (0.67 s) from frame 700, which took the first scene's dialogue through ten years of narration and into the bedroom scene.
- Walk it step by step with `DM_CTL`: `A` advances, `Y` opens the in-game menu (`Save`, `Load`, `Flowchart`, `Config`, `Control`, `Back to Title`) and `X` the backlog (with `B Back`, `A Play Voice` and `Y Quick jump`); `shot <path>` writes the next presented frame, which is how the screens above were checked. The names are the Npad buttons (`A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down`, `LLeft` … `RDown`, `T<x>x<y>`), not keyboard letters: `Return`, `U` or a bare `+` is a red line, not a press. `Plus` and `Minus` do nothing in the prologue's narration.
- The keyboard itself is [the Mac front end's](../../../docs/agents/05-debugging.md#scripted-input-and-thread-scheduling): J is A, K is B, U is X, I is Y, Return is +, Tab is −, arrows the D-pad, WASD the left stick.

## Found on this game

General host changes first needed here; each applies to every game:

- **Resolved: the fixed-function caps** a GL 1.x renderer turns off: the first run logged seven `[RED] glDisable(…)` lines before every draw; five (`0x4000`, `0xc60`–`0xc63`) survived the host's fixed pipeline and are silent now: [Fixed-function caps a GL 1.x renderer turns off](../../../docs/reference/rendering.md#fixed-function-caps-a-gl-1x-renderer-turns-off). Landed upstream as `opengl: silence unsupported fixed-function disables (PR #2995)`.

## Open

- The iPhone build is untested.
- The prologue has been played a few minutes in and saved once; a chapter past it, the config and control screens, and loading that slot back are not walked yet.
- The memory split is the survey's; the four-minute run is flat enough to leave it, and a longer one may still correct it.
