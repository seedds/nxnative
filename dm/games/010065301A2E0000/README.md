# Star Ocean: The Second Story R (`010065301A2E0000`)

Original Switch release, update 1.1.0 (v196608), SDK 15.3.2.
Unity 2021.3.22f1 IL2CPP with CRIWARE and FMOD on NVN.
Modules: `main`, `subsdk0` and `sdk`. Other versions are not validated.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `stdalloc_cap 0x60000000` with the original `0xCD500000` application pool | [game.tsv](game.tsv) | Gives later Unity reservations room in the 4.375 GiB iPhone alias region without reducing the pool reported to the game. |
| `depth_mode zero` | [game.tsv](game.tsv) | The game sets NVN near = 0; offline pipeline bases must use the same depth mode. |
| `shader_groups /Data/=data` | [game.tsv](game.tsv) | Uses the NSP script's Unity/GLSLC shader split. |

## Memory

- **Resolved on the tested iPhone layout:** With the provisional profile, two boots drew frames and then faulted at `main+0x2d9830`, writing to `0x3fffe0` after a null allocation result. The most recent alias high-water mark was +4.26 GiB in a 4.375 GiB window. The game image mounted normally. A Mac run with the provisional profile mapped beyond +4.50 GiB and reached the opening movie.
- At startup the game requests 3.14 GiB from `StandardAllocator::Initialize(nullptr, size)`. The existing [StandardAllocator cap](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space) gives it 1.50 GiB of virtual address space instead, leaving room for later regions. The 3285 MiB application pool is unchanged. The iPhone log confirmed the cap at `main+0x6df070`; startup passed the old fault, presented 289 frames by frame 300, and started the opening movie. This validates the cap on the current phone layout, not the minimum viable cap or a larger, uncapped layout.

## Rendering

- The game selects NVN near = 0 (`main0_z`). The default near = −w made the Mac reject the bundled Metal pipeline archive and rebuild the bases at first launch. `depth_mode zero` lets the packer bake the matching bases. It does not change the live depth mode or shader translation. See [offline archive depth mismatches](../../../docs/reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree).
- Mac and iPhone logs report two unimplemented NVN calls, `nvnCommandBufferSetSubpixelPrecisionBias` and `nvnCommandBufferSetConservativeRasterDilate`. No visible problem was observed on the tested route; later effects are unverified.

## Tested routes

- M5 Max, macOS 27.2 (profile with the depth fix): the bundled archive opened from a clean local cache, found 16 of 16 probed pair bases and skipped the full first-launch rebuild. The packed host loaded the copied Sacred Forest save. It still builds individual pipelines for states absent from the offline archive.
- M5 Max, macOS 27.2 (earlier build, before the archive depth fix): from a fresh save, choose Claude, advance through the Milokeenia briefing, approach the blue panel and proceed into the ruins, activate the device and reach the Sacred Forest. Clear the battle tutorial and first fight, then continue to Claude's post-battle dialogue with Rena. About 30 fps in the last measured frame window (p50 33.38 ms, p95 34.10 ms). A separate run loaded a local copy of Rena's Lv. 3 Sacred Forest autosave, moved and opened the menu; the original save was not connected.
- iPhone Duo, iOS 27 (profile tested at `d8d68385`): the signed Xcode device build passed the old startup fault and loaded the Sacred Forest autosave, with the local and iCloud copies already in sync. Ran through 3,900 frames, including movement and opening and closing the camp menu; the last 300 frames had p50 33.27 ms and p95 34.68 ms. No save changed. No IPA was exported; game data was already installed separately. A longer playthrough, a fresh iCloud download and iPad were not checked.
