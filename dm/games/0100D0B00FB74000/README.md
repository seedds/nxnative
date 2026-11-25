# XCOM 2 Collection (`0100D0B00FB74000`)

The `v131072` release has three AArch64 programs, all using SDK 9.3.1 and NVN. The launcher shows the legal pages and selects a campaign. [XCOM 2](../0100D0B00FB74001/README.md) and [War of the Chosen](../0100D0B00FB74002/README.md) use Firaxis's Unreal Engine 3 with Wwise.

| Program | Title ID | Display version | Main build |
|---|---|---|---|
| Collection launcher | `0100D0B00FB74000` | 1.0.3 | `2c430e63a17c582b986238da1ca1b6a3` |
| XCOM 2 | `0100D0B00FB74001` | 1.0.3 | `d76a84b068f61c7000c38799759c5f36` |
| War of the Chosen | `0100D0B00FB74002` | 1.0.1 | `2aa620f5acda97547bac99de9bfb8f03` |

## Packing and playing on the Mac

Build the base NSP and update `v131072` together with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app`. The development option `--platform mac` prints a run command. Program 0 opens first and `oe::ExecuteProgram` starts program 1 or 2. The profiles use the existing shared save and program-switch mechanism. On first launch, press A on each of the two legal pages, then use the directional controls and A to pick a campaign. The keyboard's A button is J.

On the Apple M6 the launcher displayed both legal pages, accepted A on each (WebView exit reason 4), and drew the selector. The combined development host handed off from the selector to each campaign and loaded its own shader library. The development builds compiled all 72,986 shaders across the three programs. Both campaigns rendered their menus and campaign setup with the Metal HUD enabled. Tactical missions and longer sessions remain unverified.

The development playtests used `DM_PSO_PREBUILD=0` to compile pipelines on demand. The expansion's complete offline prebuild of 27,444 pipeline pairs took 10 minutes 15 seconds on this Mac.

## What the profiles need

- The launcher imports GLSLC via `subsdk0` and needs no OpenGL modules. It requested a 3.14 GB heap. Its profile leaves 3.25 GB for the heap and 256 MB for stacks.
- Both campaigns reserve 8 GB at alias +64 MB through `nn::os::AllocateAddressRegion`. Their profiles allow 8.125 GB of alias space and preserve the tested heap and stack partition. macOS uses the existing 64 GB alias region. The observed reservation exceeds the iOS budget without extended virtual addressing. iOS gameplay remains open.
- The campaigns set near-depth to zero. The launcher uses the default NVN depth mode.

## Found on this game

- [Open users handed over between programs](../../../docs/reference/services.md#open-users-handed-over-between-programs): preserves the user context for the in-game campaign switch.
- [A program introduced by an NSP update has no base NCA](../../../docs/reference/packing.md#a-program-introduced-by-an-nsp-update-has-no-base-nca). War of the Chosen is wholly in `v131072` and its RomFS was extracted with strict integrity checks.
- [Unreal Engine 3 shaders in a shared buffer](../../../docs/reference/shaders.md#unreal-engine-3-shaders-in-a-shared-buffer). The same `UNIS` reader covers both campaigns, resolving 33,034 shader keys for XCOM 2 and 39,939 for War of the Chosen.
