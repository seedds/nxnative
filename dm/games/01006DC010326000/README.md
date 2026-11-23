# BRAVELY DEFAULT Ⅱ (`01006DC010326000`)

Base version 1.0.0 (`v0`), Unreal Engine 4.23, Nintendo SDK 10.4.0, NVN 53.313 and CRIWARE. The host loads `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

## Memory and graphics

- The NPDM declares 80 MiB of system resource. Unreal requests a 62 GiB address region at startup and uses the host's existing `FMallocBinned3` replacement for the slot-17 layout. With the iPhone-sized 4.375 GiB alias region on the Mac, the shared Unreal region gate grants 4.0625 GiB and the game enters its frame loop. [Unreal's engine address region](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options) · [Unreal's FMallocBinned3 replacement](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator)
- The game sets NVN depth mode to near = 0. The profile uses that mode for its offline pipelines.

## Frame rate

The profile defaults to 60 fps. `Sunrise-E/Config/DefaultGameUserSettings.ini` sets `FrameRateLimit=30.000000`, and `DefaultEngine.ini` leaves fixed-rate simulation off. Unreal already presents at interval 1. The shared rewriter locates `t.MaxFPS` by its registered name and the host updates the game and render threads' copies. The launcher also offers 30 fps. `DM_FPS_TARGET=0` uses the game's own limit. [Unreal's frame limit and present interval](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name)

In a windowed comparison on Apple M6 with macOS 27.2.1, the automatic opening reached the difficulty menu 40.6 s after choosing New Game at the stock cap and 39.4 s at 60 fps. That difference is within the roughly 1.5 s screenshot sampling interval.

Across 26 s of the opening, the stock cap averaged 29.8 fps with a median frame time of 33.33 ms. The 60 fps run averaged 58.7 fps with a median of 16.67 ms and a longest frame interval of 506 ms. The 60 fps run reached the dialogue in the first inn and exited cleanly, with zero reported audio underruns.

## Files and shaders

The version 8 `Sunrise-E-Switch.pak` stores shaders inline in Zstandard-compressed cooked packages. The split read 33,335 unique shaders (8,257 vertex, 24,566 pixel, 478 compute and 34 geometry). The supplied NSP passed the extractor's checks, all five NCA content IDs matched their SHA-256 prefixes, and the shader split reported zero read failures. [Zstandard-compressed Unreal pak entries](../../../docs/reference/shaders.md#zstandard-compressed-unreal-pak-entries-sent-to-oodle)

## Mac validation

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app` for a Finder app, or `--platform mac` for the host and run command. On a fresh save, press A at New Game, choose a difficulty, and confirm the character name. J is A on the Mac keyboard, the arrow keys navigate menus, and WASD uses the left stick.

On Apple M6 with macOS 27.2.1 and Xcode 27.2, the full shader library rendered the title screen, difficulty menu, character name confirmation, beach rescue scene and the prologue title card. The windowed checks used the iPhone-sized alias region and the Metal HUD. The software keyboard returned the scripted name `Seth`. The longest stock-cap run passed 6,600 frames and exited cleanly at its time limit. During the beach scene the median frame time was 33.4 ms (about 30 fps), and both audio streams reported zero underruns.

The prewarm seed covers the tested opening with 412 shader loads, 318 render pipelines and 17 compute pipelines. The translator reported unsupported `Sam` and `Ram` instructions in global pixel shader `9317b163ba8bb276`. The tested opening did not load that shader.

## Packages

All 33,335 shader entries passed the macOS and iOS builds. The Finder app passed strict code signature verification, and all 65 entries in the unsigned IPA passed CRC verification. Combat, save and reload, later areas, and iPhone and iPad gameplay remain untested.

## Found on this game

- BoL identified the Zstandard pak blocks and contributed the shared decoder fix. The copy used for that earlier bring-up had an unreadable texture block. [PR #3193](https://github.com/seedds/nxnative/pull/3193)
