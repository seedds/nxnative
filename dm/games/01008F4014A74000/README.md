# UnMetal (`01008F4014A74000`)

Update 1.2.0 (v262144), SDK 14.3.0 (`nnSdk-14_3_0`). Unepic Fran's own engine (its source paths name `arcadix`): a top-down stealth action game, its pictures 2D sprites and tiles, its sound Ogg Vorbis (most of the RomFS's 12,260 files).

It renders with **OpenGL 1.x fixed function through EGL**: `main` imports the EGL entry points and asks `eglGetProcAddress` for its GL functions, and `dm/tools/loader_census.py` finds 26 of them called, all of OpenGL 1.x (`glVertexPointer`, `glColorPointer`, `glTexCoordPointer`, `glEnableClientState`, `glMatrixMode`, `glLoadIdentity`, `glRotatef`, `glTexEnvi`, `glTexEnvf`, `glDrawArrays`, …) plus `EXT_framebuffer_object`. It compiles no shader. Modules `main` and `sdk`; `subsdk0` (NVIDIA's GL driver) is never loaded.

The original PR ran the logos, title and menu, intro cutscenes and first cell at 60 fps with no audio underruns on an M3 Max (macOS 27.3, Xcode 27.2). The author also reported playable iPhone gameplay without specifying the model, frame rate or IPA size. [PR #2805](https://github.com/seedds/nxnative/pull/2805)

On the rebased Mac host (M4 Pro, macOS 27.2.1, Xcode 27.2), a fresh save with subtitles off played through the intro and reached the first cell with the HUD and punch tutorial in the same process. It held about 60 fps in the cell with zero audio underruns. The original iPhone result has yet to be repeated on the rework.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | `main` takes nothing from NVIDIA's GL driver: the GL layer answers its EGL imports, so `subsdk0` stays unloaded |
| `sys_resource 0x0`; `mem_reserve 0xf0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the NPDM declares no system resource; `main` sets a heap of the pool less used memory and its 164 MB of bss. The provisional profile's values, unchanged |
| `shader_groups /=romfs` | [`game.tsv`](game.tsv) | no shaders: the NVN tables stay empty, and `gl_shaders.py --fixed-function` writes an empty GL table |

No rewriter patches, no profile key beyond these: the provisional profile the NSP script guessed booted as it was, once the guess knew a fixed-function GL game ([OpenGL 1.x fixed function](../../../docs/reference/rendering.md#opengl-1x-fixed-function)).

## Rendering (OpenGL 1.x)

- Every draw is `glDrawArrays` from client-memory arrays (`glVertexPointer` / `glColorPointer` / `glTexCoordPointer`, no buffer object), with the projection and modelview from `glMatrixMode` / `glLoadIdentity` / `glRotatef` and texture unit 0's environment from `glTexEnvi`. The fresh run stood in the first cell at about 118 draws and 9 passes a frame.
- **Passes:** the scene is drawn at 320x180 into the corner of a 1600x900 target (framebuffer 1, texture 311), scaled 5x into framebuffer 2 (texture 312) with half alpha over the previous frame, then framebuffers 3 and 4 add the frame's effects, and one quad composites texture 314 onto the 1600x900 window. A black framebuffer 1 at a swap is expected because the game clears it at the start of each frame's scene.
- **Texture environment:** the game switches unit 0 between `GL_MODULATE` and `GL_ADD` about once a frame (314 times each in 300 frames).

## Found on this game

- **Resolved: no graphics API found.** The survey said `main` imports "neither nvnBootstrapLoader nor Vulkan functions nor OpenGL through EGL", and the shader split stopped asking for a reader: the GL rule needed `glShaderSource`. Now a fixed-function GL game: [OpenGL 1.x fixed function](../../../docs/reference/rendering.md#opengl-1x-fixed-function).
- **Resolved: the fixed pipeline** (client-state arrays, the matrix stacks, `glTexEnv`, `glAlphaFunc`, `glColor*`, the `EXT` framebuffer names) in the GL layer: same entry.
- Diagnostic screenshots need to wait for the GL render queue to finish the frame. [Screenshots of GL and Vulkan frames wait for rendering](../../../docs/reference/rendering.md#screenshots-of-gl-and-vulkan-frames-wait-for-rendering)
- The black screen in the first unattended run came from the window losing focus. UnMetal processed notification 15, set its out-of-focus flag at `main+0x5f0` and skipped the game update and draws at `main+0x680` while still clearing and swapping. Injecting focus loss reproduced the black frames and restoring focus resumed draws in the same process. For unattended runs, use `DM_FOCUS_KEEP=1` as the regression harness does. [Window focus during regression runs](../../../docs/reference/frame-rate.md#a-regress-run-stops-when-another-app-takes-the-key-window)
- The fresh-save route `DM_KEYS=f360:A,f600:A,f900:A,f1200:A` accepts English, leaves subtitles off and starts a game on Easy. Add `f1330:B~1.5` to hold B and skip the intro. The skip route reached the first cell by frame 1530 on the M4 Pro.

## Open

- **Later gameplay is unverified.** Later rooms, in-game menus and combat have yet to be checked on the rework.
- **One `[RED] audio #0: the device has not called back for 212 ms while playing`** in a headless boot with the output muted (`DM_AUDIO_MUTE=1`), then 0 underruns: not looked into.
