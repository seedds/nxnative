# The Legend of Heroes: Trails into Reverie (`01008CB0156BC000`)

NIS America release, update 1.0.4 (`v262144`). PH3's Switch port of Nihon Falcom's PhyreEngine game, using NVN and Nintendo SDK 14.3.0. The loaded modules are `main`, `subsdk0` (Nintendo's movie player), and `sdk`. The host boots with the stock rewriter. The five DLC packages in the survey contain licences.

On an Apple M6 Mac (macOS 27.2.1, Xcode 27.2, 2026-11-23), the title screen, New Game on Normal and the opening Crossbell City cutscenes through field `c1400` all draw with the complete shader library. Both the development build and the standalone app reached those scenes. The audio renderer produces non-silent PCM. The app's audio device had one 228 ms callback gap during the concurrent iOS build and resumed callbacks after the host's existing retry. The title and steady cutscene intervals run around 30 fps. Scene transitions bring some 300-frame intervals below 30 fps on the first run.

## What it needs

- **Memory.** The NPDM declares zero system-resource memory. On the Mac, the game took a 2.98 GiB heap in the provisional profile's 3.25 GiB heap region at startup. The profile keeps that layout.
- **Depth.** The game calls `nvnDeviceSetDepthMode` with a near plane of zero. The profile uses `depth_mode zero` for its offline pipeline archive.
- **Shaders.** The shader split finds 58,567 keys in the merged update, comprising 14,378 vertex and 44,189 pixel stages. It reads the Phyre effects in Falcom's `.pkg` and `.pka` archives. All 58,567 build for Mac and iOS, with zero undecoded bindings in the build reports. [Falcom archive reader](../../../docs/reference/shaders.md#falcom-pkg--pka-archives-and-phyre-ryhp-glslc-effects)

## Mac route

At the title, press A, go up from Load Game to New Game and confirm with A. Choose Normal. Select No on each of the three optional import prompts to start the opening sequence. The game loads field `c0000` after the prompts.

## Memory

- **Open.** The Mac development run reached 14,720 MiB of physical footprint in the prologue. The standalone app reached 14,057 MiB. In a diagnostic run, `vmmap -summary` attributed about 9.5 GiB to `IOAccelerator (graphics)`, across about 128,000 regions.
- Both runs using the MTLDevice pipeline path and with speculative program prefetch off reached `c0000`. With normal buffer-copy ordering, the host measured 11,089 MiB in that scene. With the copy-cut diagnostic switch off, it measured 2,489 MiB there. The diagnostic switch skips the early commits and dirty-page stamps used to order texture uploads after buffer copies. We keep normal ordering in the profile and packages. [Buffer-copy ordering](../../../docs/reference/nvn-objects.md#buffer-copies-over-the-storage-of-textures-uploaded-from-it)

## Found on this game

- **Resolved.** The controller setup at `main+0xa240e4` expects two vibration handles for each FullKey slot and retains the handles by actuator position. The host originally returned zero handles for empty slots. Once enumeration was corrected, returning a position of zero for unmounted devices triggered a second assertion. [Vibration handle enumeration](../../../docs/reference/services.md#vibration-handles-before-a-controller-is-reported) and [Vibration device type and position](../../../docs/reference/services.md#vibration-device-type-and-position)

## Open

- Base version, walking and combat, save transfer from other titles, later chapters and physical controller rumble have yet to be checked.
- iPhone and iPad gameplay have yet to be checked.
