# Assassin's Creed III Remastered (`01007F600B134000`)

Switch 1 base version 1.0.0 (`v0`), Nintendo SDK 5.4.0, NVN 53.209 and Ubisoft's Scimitar / Anvil engine. The launcher and the two games load as separate NROs. The launcher displays an offline HTML page. `subsdk1` supplies GLSLC, and `subsdk2` supplies the SDK movie player.

On an Apple M6 Mac running macOS 27.2.1, Assassin's Creed III reached the cave entrance where Desmond can move. The first run created a local save, and the full shader build reloaded it and rendered the opening without `[RED]` lines on that route. Standing still at the cave entrance took a median 33.33 ms per frame (30 fps), with about 2.2 GB resident and 1.29 GB of Metal allocations. Liberation reached its title screen using the packaged Mac host and resources. Both games opened their audio outputs.

## What it needs

- The NPDM declares no system resource. The 3.25 GiB heap region, 256 MiB alias region and 256 MiB stack region cover the Mac boot and opening.
- The launcher checks the NVN version for equality. The host reads 53.209 from the bundled SDK. See [Older NVN minor versions from the SDK](../../../docs/reference/nvn-objects.md#older-nvn-minor-versions-from-the-sdk).
- The host registers 12,565 dispatch gates across the startup modules and all three NROs. See [Gate tables grow with the loaded modules](../../../docs/reference/rewriter.md#gate-tables-grow-with-the-loaded-modules).

## Shaders

The split reads 43 Scimitar v27 archives across AC III and Liberation. It finds 66,427 unique keys in the archives, NROs, other files and the two shaders compiled by GLSLC during the launcher intro. The archive reader needs `liblzo2` on the build machine. All 66,427 keys built for Mac with Metal 32023.922. See [Scimitar / Anvil forge archives](../../../docs/reference/shaders.md#control-sections-and-code-areas-in-scimitar--anvil-forge-archives).

## Mac controls

Press J (A) on the launcher title, select the game, then press J to play it. In Assassin's Creed III, press Return (+) at the title screen, then open **Single Player** and choose a save slot. Use the game window with `DM_METAL_HUD=1` to check performance during play.

## Open

- Later AC III gameplay, Liberation past its title screen and downloadable content still need playtests.
- iPhone boot, rendering and memory usage remain unverified.
