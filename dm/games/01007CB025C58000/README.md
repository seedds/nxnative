# Volcano Princess (`01007CB025C58000`)

Switch update 1.0.2 (`v131072`), Unity 6000.4.7f1 IL2CPP with Addressables and FMOD, Nintendo SDK 21.4.7, NVN 55.15. The game loads `main`, `subsdk0` (NintendoSDK_movie), and `sdk`. The SDK and movie module builds are the same as in [Silver Pines](../0100D5B026844000/README.md).

## What it needs

The [game profile](game.tsv) uses the existing host and rewriter. The NPDM declares 16 MiB of system resource, so Unity reserves and maps its own memory. With the profile's 4.375 GiB alias region enforced on the Mac, the highest mapping reached +2.51 GiB in the opening. The game chooses NVN's near = 0 depth mode. The profile uses that mode to build the pipeline archive.

The shader split finds 3,277 stages (1,578 vertex, 1,582 pixel, 116 compute, and one geometry) in the Unity data under `Data/` and the player's embedded GLSLC shaders in `main`. All 3,277 base libraries built for both Mac and iOS.

## Mac testing

On an Apple M6 Mac mini (16 GB), macOS 27.2.1 and Xcode 27.2 (27B52), a fresh save reached the title menu after the opening movie, started a new game, and reached the daughter-naming screen. The host opened the software keyboard. The game wrote its player preferences. The test recorded non-silent stereo PCM, and the audio device reported no underruns after startup. The title ran at about 60 fps (16.7 ms median frame time), and the prologue held about 1.68 GB resident and 665 MB of Metal allocations. The ad-hoc signed Mac app loaded its bundled pipeline archive and passed signature verification. The unsigned IPA passed ZIP integrity verification.

Use J (A) to select **New Game** and **Yes**. The opening movie before the title runs for about 98 seconds. In the new-game prologue, I (Y) skips to the daughter-naming prompt. From that prompt, select the name to open the software keyboard or move down to **Confirm** to keep the default.

## Open

- Name submission, later gameplay, and save reload still need a gameplay check. The test ended while the software keyboard was open.
- The rewriter's existing `ALLOC_BUCKET` matcher skipped its optional cut because the name reference did not load `x1` directly. The opening fit the configured alias region with Unity's original allocator. No game-specific cut is needed for the tested route.
- The base-only package and iPhone runtime are untested.
