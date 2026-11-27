# Thronebreaker: The Witcher Tales (`0100E910103B4000`)

Switch update 1.0.2 (v131072) over the base NSP. Unity 2018.4.2f1 IL2CPP with Wwise, SDK 8.2.1 and NVN. The Mac bring-up reached the difficulty menu and a new campaign's opening movie on an Apple M6 Mac mini.

## What it needs

| Setting | Reason |
|---|---|
| `modules main subsdk0 sdk` | `subsdk0` is NintendoSDK_movie. The SDK and movie modules match those used by [FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition](../0100CE4010AAC000/README.md). |
| 5 GiB guest block, 4.375 GiB alias region, 8 MiB system resource | The NPDM declares the system resource. The 15-second address-region check reserved up to 3.21 GiB into the alias region, including 3.14 GiB for the standard allocator. The game set no heap during the check. |
| `depth_mode zero` | The game calls `nvnDeviceSetDepthMode` with near = 0. The offline pipeline prebuild uses the matching vertex entry point. |

## Files and shaders

The merged RomFS has 13,568 files and occupies about 4.2 GiB. It contains UnityFS bundles, Wwise banks and MP4 movies under `Data/`. The splitter found 5,952 shaders (3,439 vertex, 2,475 pixel and 38 compute), including four GLSLC outputs in `main`. All 5,952 Mac and iOS libraries built with zero unresolved bindings and zero unsupported driver-bank reads. The prewarm list covers the first boot, menus and the opening movie.

## Mac route

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use the printed run command. Set `DM_METAL_HUD=1` to show the Metal HUD. From a fresh save, wait for the autosave notice, then press A to continue. Start a new game, select a difficulty and press A. The opening movie has subtitles and offers a hold-B skip. On a later run, choose Continue for Lyria to reload the campaign.

## Frame rate

The profile targets 60 fps. On 1.0.2, Unity leaves `Application.targetFrameRate` at `-1` and starts with `QualitySettings.vSyncCount` at 2. The rewriter finds both through Unity's internal-call registration. The host sets the count to 1 and presents at the profile's target. Offline discovery found both fields in the base NSP too, at different addresses. Set `DM_FPS_TARGET=30` for 30 fps. [Unity's frame-rate discovery](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table)

In the same opening dialogue in Lyria on the Apple M6 Mac mini, median frame time went from 33.15 ms at the stock setting to 16.52 ms at 60 fps. The 60 fps run had a median on-screen interval of 16.67 ms. Unity's scaled clock tracked wall time at both targets, advancing 8.09 in 8.09 seconds at 30 fps and 8.12 in 8.14 seconds at 60 fps. Both runs used the same autosave and reached the dialogue without a crash, stall or `[RED]` diagnostic.

## Validation

On the Apple M6 Mac mini with macOS 27.2.1, the initial 30 fps build reached the menus and the opening movie from a new campaign and again after reloading its autosave. The reload run presented 4,351 frames without a crash, stall or `[RED]` diagnostic. The median frame interval was about 33 ms during the opening movie, with zero underruns in the logged audio callback windows. The initial iOS build used Xcode 27.2 and Metal 32023.922.

## Found on this game

- Three of the Unity stream buffer functions matched the existing engine rule. The fourth, at `main+0x2ba7290`, stores the writer-sleep flag with `stlr` at `+0x160` and loads the reader position with `ldr` at `+0x168`. Thronebreaker adds this function's fingerprint to the existing four-function rule. A two-word assembly test on Apple M6 saw both loads read stale values in 22 of 400,000 trials with `stlr` then `ldr` and zero of 400,000 with `stlr` then `ldar`. Six unpatched 22-second game boots kept presenting through the end of the test. [Unity's stream buffer handshake loses a wake-up on Apple cores](../../../docs/reference/threads.md#unitys-stream-buffer-handshake-loses-a-wake-up-on-apple-cores)

## Open

- Field controls, card battles and later campaign areas on the Mac. The 60 fps check reached the opening dialogue in Lyria.
- iPhone and iPad runtime validation.
