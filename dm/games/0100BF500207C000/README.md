# Bloodstained: Ritual of the Night (`0100BF500207C000`)

Update 1.60 (v851968) with the base game and five DLC, Nintendo SDK 7.6.0, Unreal Engine 4 on NVN with CRIWARE audio. The modules are `main`, `subsdk0` (`NintendoSDK_movie`) and `sdk`.

## What it needs

- [`game.tsv`](game.tsv) registers 1.60 and the standard layout for a title whose NPDM declares virtual address memory. Unreal reaches more than 40 GiB into the Mac host's 64 GiB alias region during boot. The NPDM declares 70 MiB of system resources and main does not set a heap.
- The shader split uses [short v8 paks and v1 shader archives](../../../docs/reference/shaders.md#unreal-engine-short-version-8-paks-and-version-1-shader-archives). The registered tables cover 33,401 unique shaders, all built into the Mac library. The shader groups distinguish Unreal's global shaders from the game's materials, and the game's startup sets the near plane to zero.

## Mac validation

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac --package v851968` and use the printed run command. Set `DM_METAL_HUD=1` to show the Metal HUD. From the title screen choose Game Start. Start a new game or resume a Galleon Minerva save. Plus skips the illustrated prologue and the first dialogue.

On Apple M6 with macOS 27.2.1, the registered build reached the title, loaded a Galleon Minerva save, and rendered the opening dialogue and the gameplay status screen at the game's 30 fps limit.

## Found on this game

- **Resolved.** Magenta patches on both characters in the opening dialogue and on Miriam in the status screen. [Block-compressed textures with sparse layout in CPU-visible pools](../../../docs/reference/nvn-objects.md#block-compressed-textures-with-sparse-layout-in-cpu-visible-pools)

## Open

- Later stages and iPhone gameplay await testing. The iOS alias region is smaller than the range the game's memory allocator reaches on the Mac.
