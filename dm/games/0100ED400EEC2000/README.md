# STORY OF SEASONS: Friends of Mineral Town (`0100ED400EEC2000`)

Update 1.0.5 (`v327680`), Nintendo SDK 8.2.0 with NintendoWare Atk audio, and NVN API 53.312. The game runs with the standard host and the existing SDK rewriter rules. Its shaders are GLSLC outputs in `resource/shader/*.nvn`.

On the Mac (2026-11-24, Apple M6, macOS 27.2.1, Xcode 27.2), the game reached the title, New Game, character setup, and Thomas's introduction on the farm. The Mac keyboard dialog returned the character name, and the game displayed the costume unlock notice from the five supplied DLC packages. The host rendered non-silent audio and reported no `[RED]` lines or missing shaders on that route. The docked run held 60 fps in the opening farm scene at 1920×1080 (16.67 ms median frame time after the scene loaded).

## What it needs

| Setting | Why |
|---|---|
| `modules main sdk` | `main` imports from `sdk`. The host loads those two modules. |
| `mem_reserve 0xa0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | The provisional layout accounts for 1,612 MiB of `main` BSS. Startup uses 1.57 GiB of the 1.75 GiB heap region. |
| `sys_resource 0x0` | The NPDM declares zero system resource memory, so the host reports virtual address memory disabled. |
| `depth_mode zero` | The game selects near-zero depth at startup. Offline pipeline bases use the same vertex entry point as the game. |

The values are in [`game.tsv`](game.tsv). The shader split found 213 distinct shaders (109 vertex, 98 pixel, 6 geometry), all translated for the Mac. The prewarm list comes from Mac runs through the opening farm scene.

## On the Mac

Open the bundled app after building with `--platform mac-app`, or use the run command printed by `--platform mac`. Press A when the title logo is visible, select New Game, and fill in the character name, birthday and difficulty. Confirm the settings, dismiss the DLC notice if present, and press A through Thomas's dialogue to the farm-naming prompt.

## Found on this game

- **Registered texture at NVN texture pool slot 0** (depth-of-field composite sampling `tid=0` instead of the zero dummy, [#420](https://github.com/seedds/nxnative/issues/420)): [Registered textures at pool slot zero](../../../docs/reference/nvn-objects.md#registered-textures-at-pool-slot-zero).

## Open

- The Mac playtest stopped at the farm-naming prompt. Free movement, save and reload, later areas, and iPhone gameplay still need testing. Only update 1.0.5 is registered.
