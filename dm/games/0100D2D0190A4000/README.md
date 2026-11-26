# NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS (`0100D2D0190A4000`)

Switch update 1.7.1 (v720896), SDK 14.3.0, `main` build `1db3976bc6f11c208b4957b837b0781a`. CyberConnect2's game renders and presents through NVN, and uses OpenGL ES 3.0 for a second renderer that shares textures and fences with NVN.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GiB reservation, 1.25 GiB alias region, 32 MiB system resource | [`game.tsv`](game.tsv) | The game requests a 3.14 GiB heap and has virtual address memory enabled in its NPDM. |
| NVN and GLSL libraries | [`shader_map.tsv`](shader_map.tsv), [`gl_capture.tsv`](gl_capture.tsv) | The split builds 570 NVN shaders. The renderer builds 387 GLSL stages at startup, which the packer captures and translates alongside the NVN library. |
| Modules `main`, `subsdk0`, `sdk` | [`game.tsv`](game.tsv) | `subsdk0` is NintendoSDK_movie. The host answers the OpenGL imports without loading the game's NintendoSDK_OpenGL module. |

## Found on this game

- [OpenGL and NVN share textures and fences](../../../docs/reference/rendering.md#opengl-and-nvn-share-textures-and-fences). The game creates shared EGL contexts on separate threads and passes 2D textures and fences across the two APIs.
- [An ES context reported as desktop OpenGL](../../../docs/reference/rendering.md#an-es-context-reported-as-desktop-opengl). The skipped texture-limit query caused the DDS row copy to overwrite a heap link during startup.

## Mac gameplay

Verified on an Apple M6 running macOS 27.2.1 with twelve DLC packages available. From Game Mode Select, use the left stick (W/S on the Mac keyboard) to select **Free Battle**. Choose **VS Battle**, confirm **P1 VS CPU**, then select both teams and a stage. On the keyboard, J confirms and K goes back.

An offline team battle reached active play at 1600 × 900 and about 30 fps. The standing-still test exercised opponent attacks, support characters and pausing. The battle HUD showed intermittent rectangular artifacts around some portraits. The host also reported one skipped NVN upload for mip level 6 on a texture with six levels. The relation between these observations is open. The Mac app and unsigned IPA both contain the two shader libraries. Other modes, online play and physical iOS devices remain unverified.
