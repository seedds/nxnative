# Guacamelee! 2 (`01007E100456C000`)

Switch 1 update 1.0.4 (`v262144`), Nintendo SDK 5.3.0, NVN API 53.313. DrinkBox Studios' engine uses NVN graphics and FMOD sound banks. The host loads `main` and `sdk`.

On the Mac, a fresh save reached the video setup screen, the title and save-slot menus, and the opening scene. On the Apple M6, the opening scene at rest had a median frame time of 16.67 ms (60 fps), about 1.4 GiB resident memory, and 723 MB of Metal allocations. The game opened stereo audio through `AudioOut`, committed its save, and detected both DLC licenses. No `[RED]` lines appeared on this route.

The packaged Mac host loaded its bundled pipeline archive and produced non-silent stereo PCM in a separate run with a copy of the test save.

## What it needs

| Setting | Why |
|---|---|
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | The NPDM declares no system resource. The SDK requests a 3.14 GB heap at boot, which fits the 3.25 GB heap region. |
| `depth_mode zero` | The game calls `nvnDeviceSetDepthMode` with near = 0. The pipeline prebuild uses the same vertex shader entry point as the live renderer. |

The existing host and rewriter handle this build without per-build cuts. See the [profile](game.tsv).

## Shaders

`shaders.dat.nx` contains 425 unique shaders (132 vertex and 293 pixel). All compiled for Mac and iOS. The engine pairs separable stages at run time. The split makes a cover of 660 vertex/pixel pairs for prebuild. See [DrinkBox's SHADERS container](../../../docs/reference/shaders.md#drinkboxs-shaders-container).

## Mac controls

On a fresh save, press J (A) to confirm the video setup. Choose **Start**, select an empty slot, and continue through the opening. J advances dialogue, WASD moves, K is B, and Return is +. The Mac performance overlay is enabled with `DM_METAL_HUD=1`.

## Open

- Movement, combat, local co-op and playing the DLC still need a gameplay check.
- The unsigned iOS build needs an iPhone test. The earlier Switch builds have no gameplay check.
