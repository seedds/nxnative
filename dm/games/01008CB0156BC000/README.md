# The Legend of Heroes: Trails into Reverie (`01008CB0156BC000`)

NIS America release, update 1.0.4 (`v262144`). PH3's Switch port of Nihon Falcom's PhyreEngine game, using NVN and Nintendo SDK 14.3.0. The loaded modules are `main`, `subsdk0` (Nintendo's movie player), and `sdk`. The five DLC packages in the survey contain licences.

On an Apple M6 Mac (macOS 27.2.1, Xcode 27.2, 2026-11-23), the title screen, New Game on Normal and the opening Crossbell City cutscenes through field `c1400` all draw with the complete shader library. Both the development build and the standalone app reached those scenes. The audio renderer produces non-silent PCM. The app's audio device had one 228 ms callback gap during the concurrent iOS build and resumed callbacks after the host's existing retry.

## What it needs

- **Memory.** The NPDM declares zero system-resource memory. On the Mac, the game took a 2.98 GiB heap in the provisional profile's 3.25 GiB heap region at startup. The profile keeps that layout.
- **Depth.** The game calls `nvnDeviceSetDepthMode` with a near plane of zero. The profile uses `depth_mode zero` for its offline pipeline archive.
- **Shaders.** The shader split finds 58,567 keys in the merged update, comprising 14,378 vertex and 44,189 pixel stages. It reads the Phyre effects in Falcom's `.pkg` and `.pka` archives. All 58,567 build for Mac and iOS, with zero undecoded bindings in the build reports. [Falcom archive reader](../../../docs/reference/shaders.md#falcom-pkg--pka-archives-and-phyre-ryhp-glslc-effects)

## Mac route

At the title, press A, go up from Load Game to New Game and confirm with A. Choose Normal. Select No on each of the three optional import prompts to start the opening sequence. The game loads field `c0000` after the prompts.

## Frame rate

- The stock game asks NVN to present at interval 1. On the Mac the display link runs at 60 Hz, and the game sends about 30 frames per second. The update callback at `main+0x81910` in 1.0.4 accumulates elapsed seconds and schedules camera and render updates every 1/30 s. Two animation deltas in that callback also use 1/30 s as their time base.
- The rewriter schedules updates every 1/60 s and uses the game's measured elapsed time for the animation deltas, clamped to 1/60–1/15 s before applying their existing time scales. One atomic function rule covers the complete callback and its direct callees. It matches at `main+0x80c20` on the cartridge build and `main+0x81910` on 1.0.4. Both Final Fantasy XII PhyreEngine mains on hand have no match. A future Reverie build whose timing code or direct callees change needs another reviewed fingerprint.
- On the M6 Mac, 1.0.4's median frame time went from 33.31 ms in the stock title menu to 16.67 ms in the standalone 60 fps app. The steady `c0000` cutscene ran at 16.67 ms median and 16.80–16.87 ms p95. Asset loads still cause dips. From the first `c0000` asset load to the `c1400` load, the stock test took 57.56 s and the 60 fps app took 58.29 s, including load stalls. The rewritten ARM64 timing blocks from v0 and 1.0.4 both passed offline checks for slow frames and non-default animation scales.
- MasaGratoR identified the update callback and reported a text fade issue above 30 fps in the “Beyond the Reverie” dream. That scene remains untested here. [FPSLocker analysis](https://github.com/masagrator/FPSLocker-Warehouse/blob/v4/SaltySD/plugins/FPSLocker/patches/01008CB0156BC000/7735C8DD89D145F2.yaml)

## Memory

- **Open.** The Mac development run reached 14,720 MiB of physical footprint in the prologue. The standalone app reached 14,057 MiB. In a diagnostic run, `vmmap -summary` attributed about 9.5 GiB to `IOAccelerator (graphics)`, across about 128,000 regions.
- Both runs using the MTLDevice pipeline path and with speculative program prefetch off reached `c0000`. With normal buffer-copy ordering, the host measured 11,089 MiB in that scene. With the copy-cut diagnostic switch off, it measured 2,489 MiB there. The diagnostic switch skips the early commits and dirty-page stamps used to order texture uploads after buffer copies. We keep normal ordering in the profile and packages. [Buffer-copy ordering](../../../docs/reference/nvn-objects.md#buffer-copies-over-the-storage-of-textures-uploaded-from-it)

## Found on this game

- **Resolved.** The controller setup at `main+0xa240e4` expects two vibration handles for each FullKey slot and retains the handles by actuator position. The host originally returned zero handles for empty slots. Once enumeration was corrected, returning a position of zero for unmounted devices triggered a second assertion. [Vibration handle enumeration](../../../docs/reference/services.md#vibration-handles-before-a-controller-is-reported) and [Vibration device type and position](../../../docs/reference/services.md#vibration-device-type-and-position)

## Open

- Base version, walking and combat, save transfer from other titles, later chapters and physical controller rumble have yet to be checked.
- iPhone and iPad gameplay have yet to be checked.
