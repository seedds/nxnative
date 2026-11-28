# NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS (`0100D2D0190A4000`)

Switch update 1.7.1 (v720896), SDK 14.3.0, `main` build `1db3976bc6f11c208b4957b837b0781a`. CyberConnect2's game renders and presents through NVN, and uses OpenGL ES 3.0 for a second renderer that shares textures and fences with NVN.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GiB reservation, 1.25 GiB alias region, 32 MiB system resource | [`game.tsv`](game.tsv) | The game requests a 3.14 GiB heap and has virtual address memory enabled in its NPDM. |
| NVN and GLSL libraries | `shader_map.tsv`, [`gl_capture.tsv`](gl_capture.tsv) | The split builds 570 NVN shaders. The renderer builds 387 GLSL stages at startup, which the packer captures and translates alongside the NVN library. |
| Modules `main`, `subsdk0`, `sdk` | [`game.tsv`](game.tsv) | `subsdk0` is NintendoSDK_movie. The host answers the OpenGL imports without loading the game's NintendoSDK_OpenGL module. |

## Found on this game

- [OpenGL and NVN share textures and fences](../../../docs/reference/rendering.md#opengl-and-nvn-share-textures-and-fences). The game creates shared EGL contexts on separate threads and passes 2D textures and fences across the two APIs.
- [An ES context reported as desktop OpenGL](../../../docs/reference/rendering.md#an-es-context-reported-as-desktop-opengl). The skipped texture-limit query caused the DDS row copy to overwrite a heap link during startup.

## Frame rate

The game starts at 30 fps even though it asks NVN to present every refresh. [`patches.py`](patches.py) passes 60 to the game's own startup rate setter, which selects the engine's corresponding update cadence. Forcing that setting alone stalls the boot logos because `ccSceneBootLogo` truncates the elapsed half tick to zero before adding it to its clock. The patch counts half ticks and scales both the logo duration and the button skip threshold to match. The [FPSLocker configuration for 1.7.1](https://github.com/masagrator/FPSLocker-Warehouse/blob/HEAD/SaltySD/plugins/FPSLocker/patches/0100D2D0190A4000/1DB3976BC6F11C20.yaml) identifies the native 60 fps startup setting.

The shipped Scaleform battle-stage selector can ignore buttons with a 60 Hz update. A full 30 Hz timeline tick restored input in a live comparison. The patch selects the game's original 30 fps mode for the stage selector and restores 60 when the player confirms a stage or goes back. The rewriter applies the complete change when the startup, logo, stage entry, stage update and native rate setter functions match. The fingerprints cover 1.0.0 and 1.7.1.

## Mac gameplay

Verified on an Apple M6 running macOS 27.2.1 with twelve DLC packages available. From Game Mode Select, use the left stick (W/S on the Mac keyboard) to select **Free Battle**. Choose **VS Battle**, confirm **P1 VS CPU**, then select both teams and a stage. On the keyboard, J confirms and K goes back.

An offline team battle reached active play at 1600 × 900 and about 30 fps. The standing-still test exercised opponent attacks, support characters and pausing. The battle HUD showed intermittent rectangular artifacts around some portraits. The host also reported one skipped NVN upload for mip level 6 on a texture with six levels. The relation between these observations is open. The Mac app and unsigned IPA both contain the two shader libraries. Other modes, online play and physical iOS devices remain unverified.

The 60 fps diagnostic build reached 59.3–60.0 fps at 1600 × 900 in a standing-still VS Battle on the same Apple M6, with the silent audio sink. A separate real-audio run reached combat after the full-tick input probe. The final stage-specific 30 fps transition was rewritten and checked by disassembly on both builds. Its gameplay retest was skipped.
