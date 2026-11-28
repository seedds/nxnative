# Assassin's Creed III Remastered (`01007F600B134000`)

Switch 1 base version 1.0.0 (`v0`), Nintendo SDK 5.4.0, NVN 53.209 and Ubisoft's Scimitar / Anvil engine. The launcher and the two games load as separate NROs. The launcher displays an offline HTML page. `subsdk1` supplies GLSLC, and `subsdk2` supplies the SDK movie player.

On an Apple M6 Mac running macOS 27.2.1, Assassin's Creed III reached the cave entrance where Desmond can move. The first run created a local save, and the full shader build reloaded it and rendered the opening without `[RED]` lines on that route. The original 30 fps run used about 2.2 GB resident memory and 1.29 GB of Metal allocations at the cave. Both games opened their audio outputs.

## What it needs

- The NPDM declares no system resource. The 3.25 GiB heap region, 256 MiB alias region and 256 MiB stack region cover the Mac boot and opening.
- The launcher checks the NVN version for equality. The host reads 53.209 from the bundled SDK. See [Older NVN minor versions from the SDK](../../../docs/reference/nvn-objects.md#older-nvn-minor-versions-from-the-sdk).
- The host registers 12,565 dispatch gates across the startup modules and all three NROs. See [Gate tables grow with the loaded modules](../../../docs/reference/rewriter.md#gate-tables-grow-with-the-loaded-modules).

## Frame rate

In AC III 1.0.0, `DisplayOptions::Init` in `nro0` sets NVN's present interval to 2 (`+0x85bc4c`). Liberation initializes it to 1 in `nro1` (`+0x83d90c`). Both games start in elapsed-time mode in `SystemLoop::UpdateSystemClock`, which computes the simulation step from the change in CPU ticks. Liberation's `ExoticEagle` state selects a fixed 24 fps step and the engine waits on its own tick deadline in that mode.

The [profile](game.tsv) uses the host's [display pacing](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode). The launcher's **Frame rate** defaults to 60 fps and also offers 30 fps. The setting is shared by both games and works at the presentation boundary. Both engines keep their own simulation clocks.

On the M6, the AC III opening cave held 59–60 fps at the new cap, with a median frame time of 16.66 ms versus 33.33 ms with the original interval. The NPCs took about nine seconds to walk into the cave at either cap when loading the same save and leaving Desmond still. In the steady cave, the host reported about 1.6 W of CPU power at 60 fps, versus 0.8–0.9 W at the original 30 fps.

Liberation reached the Abstergo introduction with its original interval and with the 60 fps cap. Its menu was already near 60 fps. The introductory sequence still produced long frames in both runs.

## Shaders

The split reads 43 Scimitar v27 archives across AC III and Liberation. It finds 66,427 unique keys in the archives, NROs, other files and the two shaders compiled by GLSLC during the launcher intro. The archive reader needs `liblzo2` on the build machine. All 66,427 keys built for Mac with Metal 32023.922. See [Scimitar / Anvil forge archives](../../../docs/reference/shaders.md#control-sections-and-code-areas-in-scimitar--anvil-forge-archives).

## Mac controls

Press J (A) on the launcher title, select the game, then press J to play it. In Assassin's Creed III, press Return (+) at the title screen, then open **Single Player** and choose a save slot. Use the game window with `DM_METAL_HUD=1` to check performance during play.

## Open

- Later AC III gameplay, Liberation after the Abstergo introduction and downloadable content still need playtests.
- iPhone boot, rendering and memory usage remain unverified.
