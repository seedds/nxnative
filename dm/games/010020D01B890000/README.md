# Apollo Justice: Ace Attorney Trilogy (`010020D01B890000`)

Update 1.0.1 (`v65536`) over the base NSP. The collection uses Capcom's RE Engine on NVN, Nintendo SDK 15.3.2, Havok and Wwise. It loads `main`, `subsdk0` (NintendoSDK_movie) and `sdk` with the existing host and rewriter.

On an Apple M6 (macOS 27.2.1, Xcode 27.2), the collection created a save, accepted English in the language menu and rendered the title, all three game selections and the Museum menu. Apollo Justice: Ace Attorney played the Turnabout Trump intro movie and rendered the first playable dialogue in the courthouse lobby. Wwise started a six-channel output. The smoke test had no `[RED]` lines.

## What it needs

- The [profile](game.tsv) leaves 3.25 GiB for the heap, which covers the 3.14 GiB request seen at boot. The NPDM declares zero system resource memory.
- The game sets near depth to zero at NVN device initialization. The profile carries that mode for offline pipeline building.
- RomFS contains `natives/NSW/runtime.args`, `re_chunk_000.pak` and `re_chunk_000.pak.patch_001.pak`. The existing [RE Engine SDF reader](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks) found 9,717 shader keys and 11,173 vertex and pixel pairs. All 9,717 shaders compiled for Mac with Metal 32023.922 on chain `d1d792adc17b`.

## On the Mac

Build the base NSP and update together with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app`. On first boot, dismiss the autosave notice and choose a language. Choose **Game Selection & Museum** at the title, use L and R to select a game, and choose **New Game**. The Mac keyboard's A button is J.

## Open

Only 1.0.1 was tested. Dual Destinies and Spirit of Justice were checked at their menus. Later gameplay, other Museum screens and iPhone gameplay remain unverified.
