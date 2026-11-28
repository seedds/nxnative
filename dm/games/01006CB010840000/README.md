# Oceanhorn 2: Knights of the Lost Realm (`01006CB010840000`)

Update 1.1 (`v65536`), Unreal Engine 4.24, Nintendo SDK 10.4.1 and NVN 53.313. The host loads `main` and `sdk`.

## Memory

The NPDM declares 80 MiB of system resources. Unreal requests a 62 GiB address region at startup. The shared region gate grants 4.0625 GiB with the profile's 4.375 GiB alias region, and the existing slot-17 `FMallocBinned3` replacement uses a host malloc zone. The Mac smoke test uses this iPhone-sized address layout. [Unreal's engine address region](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options) · [Unreal's FMallocBinned3 replacement](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator)

## Shaders

`OH2/Content/Paks/OH2-Switch.pak` uses the five-slot version 8 footer. Its two version 1 shader archives hold 276 global and 4,220 game records. The split produces 4,442 unique shaders (1,474 vertex and 2,968 pixel). The game sets NVN depth mode to near = 0, and the profile builds pipelines for that mode.

## Frame rate

The profile targets 60 fps through the shared Unreal timing code. The rewriter finds `rhi.SyncInterval` by its registered name. The host sets both copies to one vsync per frame, down from two at the stock cap. The launcher offers 30 fps. Set `DM_FPS_TARGET=0` in a direct host run to use the game's original timing. [Unreal's frame limit and present interval](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name)

On Apple M6 with macOS 27.2.1, the opening cinematic reached matching shots at the same wall-clock samples in the stock and 60 fps runs. The same 20-second segment averaged 29.9 fps at stock and 59.4 fps at the 60 fps target, with median frame times of 33.36 and 16.73 ms.

Both caps loaded a copy of the first dock save and showed the gameplay HUD by the 74-second capture. The 60 fps run produced 59.8 fps in a five-second sample just after that capture. It averaged 50.0 fps with a 16.67 ms median frame time over a later 16-second sample that included movement away from the dock and intermittent stalls. The run logged 44 audio underruns. Another game's Metal pipeline prebuild was active during the dock runs.

## Mac verification

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app` for the Finder app, or use `--platform mac` for the development host. At the title screen, press A, then confirm New Game in an empty save slot. J is A on the Mac keyboard, WASD moves and Shift plus the arrow keys control the right stick.

On Apple M6 with macOS 27.2.1 and Xcode 27.2, a fresh save in a window with the Metal HUD reached the first dock after the opening cinematic. Standing there for 1,800 frames took 59.9 s and gave a median frame time of 33.3 ms and a p95 of 37.2 ms at the stock cap. The run peaked at 2,083 MiB of reported footprint and settled near 1,996 MiB. It passed 6,000 frames, reported zero audio underruns or red lines, and exited cleanly. Metal compiled pipelines on first use during scene transitions.

## Packages

All 4,442 shaders built for macOS and iOS. Both launchers default to 60 fps and offer a 30 fps option. The Mac app passed strict code-signature verification, and all 80 entries in the unsigned IPA passed ZIP CRC verification.

## Found on this game

- **Resolved.** The shader split skipped the pak's shared shader archives. [Shared shader archives in five-slot Unreal version 8 paks](../../../docs/reference/shaders.md#shared-shader-archives-in-five-slot-unreal-version-8-paks)

## Open

- Later gameplay, saving and reloading beyond the first dock, and iPhone and iPad gameplay await verification.
