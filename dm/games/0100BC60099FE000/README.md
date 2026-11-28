# Iconoclasts (`0100BC60099FE000`)

Switch update 1.15p (`v65536`), Nintendo SDK 5.3.0, NVN API 53.209. The game uses its own engine and loads `main` and `sdk`.

The Mac test used a fresh save and real stereo audio. It reached the title menu, started a new game in Standard Mode, played the opening cutscene, and reached Robin's room with the gameplay HUD. The game committed save data during the opening. The test used an Apple M6 Mac mini (16 GB), macOS 27.2.1 and Xcode 27.2 (27B52).

At rest in Robin's room, the median frame time was 16.67 ms (60 fps). Over 150 seconds, resident memory moved from 1,163 to 1,161 MB and Metal allocations stayed at 334 MB. The title and opening cutscene produced non-silent stereo PCM.

The standalone Mac app loaded its bundled pipeline archive and reached Robin's room. All 182 base shader libraries built for Mac and iOS, and the unsigned IPA passed its archive integrity check.

## What it needs

The existing host and rewriter run this build with the [game profile](game.tsv). The NPDM declares zero system resource, so the guest has no virtual address memory. The loader counts `main`'s 582 MiB of bss as used memory. The SDK's 2.58 GiB heap request fits in the profile's 2.75 GiB heap region.

The existing shader split finds 91 GLSLC vertex/pixel pairs in `Assets.dat`. The game selects the upper-left window origin and the host presents a 1280 × 720 swap chain. The [prewarm list](pso_manifest.tsv) covers the title, opening cutscene and first room.

## Mac controls

On a fresh save, let the title menu appear. Press Down to select **New Game**, then J (A) to confirm. Press J again to select **Standard Mode**. The opening cutscene leads to Robin's room. WASD maps to the left stick, the arrow keys map to the D-pad, and J/K/U/I map to A/B/X/Y. Use `DM_METAL_HUD=1` for the Mac performance HUD.

## Open

- Later rooms, combat and checkpoint reload still need a gameplay check. The opening-room run wrote save data and left the first load slot empty.
- The base-only package and iPhone runtime are untested.
