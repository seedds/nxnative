# Love Is All Around 2 (`0100D2C024D06000`)

Update 1.16.0.223 (v131072), SDK 19.3.14. Unity 2022.3.62f1 IL2CPP with FMOD on NVN. Loads `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The SDK and movie module are the same builds as [Cult of the Lamb](../01002E7016C46000/README.md). The opening story plays on the Mac with English subtitles.

## Memory

- The NPDM declares 16 MiB of system resources. At `main+0xa06dbc`, startup requests 3.14 GiB of virtual address space for StandardAllocator. The [profile](game.tsv) has a 4.375 GiB alias region for iOS. The SDK's existing rule shrinks the [address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area) to 62 MiB for the smaller layout.
- **Resolved on Mac with the iPhone address layout.** The title's language dialog intermittently drew blocks in place of letters. In a failing run, `VammManager::AllocateAddressRegion` returned `0x1803` at `sdk+0x142608` for Unity requests of `0x103f000` and `0x83f000` bytes from `main+0x14a6c2c`. The allocator found no aligned free span large enough. The following TextMeshPro draw (`786b683c6549ad58`) sampled a 512 × 512 UI texture. The same draw on the Mac's 64 GiB layout sampled the correct 4096 × 4096 R8 font atlas.
- The existing [StandardAllocator cap](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space) gives this title 2.5 GiB at startup and leaves about 0.64 GiB more address space for Unity's later reservations. This accommodates the smaller address space available on iOS. With `DM_ALIAS_SIZE=0x118000000` on the Mac, the allocator probes saw no failures at the dialog, the draw used the 4096 × 4096 atlas, and the text was readable with prewarm disabled.

## Files, shaders and audio

- RomFS holds 3,318 files, including 1,028 UnityFS bundles and 482 WebM clips. The shader splitter reads Unity's assets under `Data/` and the player's GLSLC outputs in `main`. All 1,686 shaders built on the Mac. The game sets NVN depth near to zero and uses present interval 1. The prewarm list holds 12 render pipelines recorded through the opening story.
- The opening clips are 1280 × 720 VP8 with two 48 kHz Vorbis stereo tracks. The English opening played through the existing `movie::Decoder` hooks and FMOD sent the stereo mix to AudioOut. English, Japanese, Simplified Chinese and Traditional Chinese are listed in the package metadata.

## Mac playtest

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`, then run the command it prints with `DM_METAL_HUD=1`. With a fresh save, A skips the trailer, confirms the system language and starts the game at the title screen. The story overlay shows X for Pause, ZL for Notes and ZR for Bag.
- 2026-11-28, Apple M6 with macOS 27.2.1. A windowed run from a fresh save reached the opening story with English subtitles and stereo output. The 3,600 frames after frame 600 had a 16.67 ms median and 16.76 ms p95 frame time.
- The packaged Mac app displayed the language dialog and started the opening story with the HUD enabled. It loaded all 12 prewarm pipelines from its archive in 0.03 seconds.

## Open

- Later chapters, save and continue, other languages and gameplay on an iPhone remain to be checked. Only update 1.16.0.223 is registered.
