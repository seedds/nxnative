# DRAGON BALL Z: KAKAROT + A NEW POWER AWAKENS SET (`010051C0134F8000`)

Update 1.70 (`v1048576`), Unreal Engine 4.24, Nintendo SDK 9.3.1, NVN 53.313 and CRIWARE audio and movies. The executable imports from `sdk` and `subsdk0` (NintendoSDK_movie). The merged RomFS contains 8,546 files, about 22.6 GiB. The supplied 17 DLC licence packages extract successfully and the Mac host reports all 17 to the game.

## Mac verification

On Apple M6 with macOS 27.2.1 and Xcode 27.2, a fresh save reached the language selector, opening movie and version 1.70 title screen. The player continued through New Game into the opening Piccolo fight. Its arena, characters, effects and battle HUD rendered, with a median frame time of about 33.4 ms. The windowed test used the Metal HUD and the profile's iPhone-sized alias region. iPhone gameplay for 1.70, later chapters and DLC gameplay remain open.

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app` for a Finder app, or `--platform mac` for the host and run command. From a fresh save, choose a language, review the first-run agreement, and select New Game. On the Mac keyboard J is A, Return is Plus, WASD moves, and Shift plus arrow keys move the right stick.

## iPhone verification of the base executable

@dag294-alt independently identified the slot-17 allocator layout and tested their patch on an iPhone 18 Pro with iOS 27.2.1, Cartouche 0.13.1 and Increased Memory Limit enabled. The unpatched game crashed during allocator startup. With the host allocator active, the game ran for about 11.5 minutes through the opening tutorial. At frame 20,400, median frame time over the preceding 300 frames was 33.46 ms and the memory footprint was 2,262 MiB. The log ended with a graceful exit. [PR #3104](https://github.com/seedds/nxnative/pull/3104) · [iPhone log](https://gist.github.com/dag294-alt/7484f97dfe798ec679661c635ab4ac0f#file-kakarot-iphone-after-txt)

The PR reports package `v983040`. The Mac and iPhone logs identify `main` by the prefix `98217279`, which matches the 1.00 base NSP's executable (`98217279aaf870fadab6445062e2f36d`). The iPhone result applies to that executable. [Contributor logs](https://gist.github.com/dag294-alt/7484f97dfe798ec679661c635ab4ac0f)

## Memory

`nninitStartup` reserves a 62 GiB address region at `main+0x24b3194`. `GCreateMalloc` calls the `FMallocBinned3` constructor at `main+0x24b37b8`. The constructor (`main+0x235c4a0`) stores its vtable (`main+0x8a181e8`) and asks the engine's virtual allocator for `0x940000000` bytes (37 GiB). With the region capped to 4.06 GiB, the old host crashed during the resulting out-of-memory report at `main+0x42f6104`, loading from address `0x30c0` with a null base.

The existing engine-wide rewriter recognizes the allocator by its name and constructor and now accepts getter slot 17. The host uses the measured 18-slot allocator interface. During a headless Mac boot with the profile's 4.375 GiB alias region, the patched game reached `nnMain` and presented frames. It reserved 4.06 GiB for Unreal, and opened addresses within the first 512 MiB of the alias region during the 23-second check. This verifies the iPhone-sized virtual address layout on the Mac. [Unreal's FMallocBinned3 replacement](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator)

## Shaders

The two version 8 paks use Zlib and store shaders inline in cooked packages. The engine's global shaders are in `Engine/GlobalShaderCache-GLSL_SWITCH.bin`. The split found 75,430 shaders (9,650 vertex, 65,182 pixel, 595 compute and 3 geometry). The game sets NVN depth mode to near = 0, and asks for present interval 2 after startup. [Unreal Engine 4.22 to 4.25 paks](../../../docs/reference/shaders.md#unreal-engine-422-to-425-paks-version-8)

The full Mac build reported 75,430 of 75,430 shaders ready, with no failures or reported losses. The prewarm list records 563 shader entries, 408 render pipelines and six compute pipelines from setup through the opening fight.

## Found on this game

- Unreal Engine 4.24's 18-slot allocator layout, identified by the `Binned3` name getter at slot 17. [Unreal's FMallocBinned3 replacement](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator)
