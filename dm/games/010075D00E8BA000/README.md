# Alien: Isolation (`010075D00E8BA000`)

Switch update `1.1.5_64113` (`v327680`), Nintendo SDK 8.2.0 and NVN 53.312. Creative Assembly's Cathode engine uses Havok and Wwise. The standard rewriter and host run `main`, NintendoSDK_movie (`subsdk0`), GLSLC (`subsdk1`) and `sdk`.

On an Apple M6 with macOS 27.2.1, the game rendered the opening movies, title screen and first playable scene aboard the Torrens at 1920 × 1080. It held roughly 3.2 GB of resident memory aboard the Torrens. Wwise opened a six-channel AudioOut stream with real output. The route logged one 203 ms Metal residency commit and no missing shaders or crashes.

## What it needs

| Setting | Why |
|---|---|
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | The NPDM declares zero system resource and the SDK requests a 3.14 GB heap. The profile leaves 3.25 GB for the heap. |
| `depth_mode zero` | The game selects zero near depth. The pipeline prebuild uses the same vertex entry point as the live renderer. |

## Frame rate

The Switch renderer requests a two-refresh NVN present interval aboard the Torrens even though the startup preferences already contain `MaxFramesPerSecond=60`. The Switch startup code writes that preference itself. The profile uses the host's existing `present_interval display` mode with a `fps_target` of 60. This works with existing saves and has no dependency on executable addresses. The launcher's Frame rate setting can select 30 fps.

In a 15-second stationary sample aboard the Torrens on the M6, the median on-screen interval went from 33.33 ms with stock pacing to 16.67 ms with display pacing. The game submitted 30.1 and 61.1 frames per second respectively. Display pacing showed 59.5 frames per second.

## Shaders

The existing GLSLC reader extracts 3,393 distinct shaders from `resources/nvn_shader_cache` (`NVNC`). Six more compute shaders are compiled during boot and saved by the packer's Mac capture. The same six were reproduced in a separate windowed run. All 3,399 stages compiled for Mac and iOS with Xcode 27.2.

## Mac controls

Press Q and E together for L + R at the title screen. J confirms, K goes back, WASD moves and Shift + arrow keys control the right stick. Set `DM_METAL_HUD=1` to show the performance HUD.

## Open

- Camera feel and animation timing at 60 fps, later missions, add-on content and on-device iOS gameplay still need testing.
