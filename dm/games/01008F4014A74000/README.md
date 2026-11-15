# UnMetal (`01008F4014A74000`)

Update 1.2.0 (v262144), SDK 14.3.0 (`nnSdk-14_3_0`). Unepic Fran's own engine (its source paths name `arcadix`): a top-down stealth action game, its pictures 2D sprites and tiles, its sound Ogg Vorbis (most of the RomFS's 12,260 files).

It renders with **OpenGL 1.x fixed function through EGL**: `main` imports the EGL entry points and asks `eglGetProcAddress` for its GL functions, and `dm/tools/loader_census.py` finds 26 of them called, all of OpenGL 1.x (`glVertexPointer`, `glColorPointer`, `glTexCoordPointer`, `glEnableClientState`, `glMatrixMode`, `glLoadIdentity`, `glRotatef`, `glTexEnvi`, `glTexEnvf`, `glDrawArrays`, …) plus `EXT_framebuffer_object`. It compiles no shader. Modules `main` and `sdk`; `subsdk0` (NVIDIA's GL driver) is never loaded.

On the Mac: the logos, the title and menu, the intro cutscenes with their text, and the first cell (the HUD, moving, the tutorial's hints) at 60 fps with sound (0 underruns). On the iPhone: tested by the user, it plays.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | `main` takes nothing from NVIDIA's GL driver: the GL layer answers its EGL imports, so `subsdk0` stays unloaded |
| `sys_resource 0x0`; `mem_reserve 0xf0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the NPDM declares no system resource; `main` sets a heap of the pool less used memory and its 164 MB of bss. The provisional profile's values, unchanged |
| `shader_groups /=romfs` | [`game.tsv`](game.tsv) | no shaders: the NVN tables stay empty, and `gl_shaders.py --fixed-function` writes an empty GL table |

No rewriter patches, no profile key beyond these: the provisional profile the NSP script guessed booted as it was, once the guess knew a fixed-function GL game ([OpenGL 1.x fixed function](../../../docs/reference/rendering.md#opengl-1x-fixed-function)).

## Rendering (OpenGL 1.x)

- **Current:** every draw is `glDrawArrays` from client-memory arrays (`glVertexPointer` / `glColorPointer` / `glTexCoordPointer`, no buffer object), with the projection and modelview from `glMatrixMode` / `glLoadIdentity` / `glRotatef` and texture unit 0's environment from `glTexEnvi`. About 157 draws and 9 passes a frame in the first cell.
- **Passes:** the scene is drawn at 320x180 into the corner of a 1600x900 target (framebuffer 1, texture 311), scaled 5x into framebuffer 2 (texture 312) with half alpha over the previous frame, then framebuffers 3 and 4 add the frame's effects, and one quad composites texture 314 onto the 1600x900 window. A draw log (`DM_GL_DRAWLOG`) that shows a black framebuffer 1 at a swap is expected: the game clears it at the start of each frame's scene; `DM_GL_TEXDUMP` dumps each target's picture.
- **Texture environment:** the game switches unit 0 between `GL_MODULATE` and `GL_ADD` about once a frame (314 times each in 300 frames).

## Found on this game

- **Resolved: no graphics API found.** The survey said `main` imports "neither nvnBootstrapLoader nor Vulkan functions nor OpenGL through EGL", and the shader split stopped asking for a reader: the GL rule needed `glShaderSource`. Now a fixed-function GL game: [OpenGL 1.x fixed function](../../../docs/reference/rendering.md#opengl-1x-fixed-function).
- **Resolved: the fixed pipeline** (client-state arrays, the matrix stacks, `glTexEnv`, `glAlphaFunc`, `glColor*`, the `EXT` framebuffer names) in the GL layer: same entry.
- **Resolved: black screenshots of gameplay** that the screen showed whole (`DM_NVN_SHOT` every 20 frames: 82 of 86 black): [Screenshots of a GL or Vulkan frame wait for its producer](../../../docs/reference/rendering.md#screenshots-of-a-gl-or-vulkan-frame-wait-for-its-producer).
- **`DM_GL_TEXDUMP`** (every framebuffer's picture at chosen frames) was added for the screenshots above: see [env_switches.tsv](../../../docs/env_switches.tsv).

## Open

- **Verified on the Mac up to the first cell**: the cutscenes, the HUD, moving and the first tutorial hints. Later rooms, the menus in play, the save flow and combat are unverified.
- **One `[RED] audio #0: the device has not called back for 212 ms while playing`** in a headless boot with the output muted (`DM_AUDIO_MUTE=1`), then 0 underruns: not looked into.
