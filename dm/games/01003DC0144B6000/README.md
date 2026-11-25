# Outer Wilds (`01003DC0144B6000`)

Switch update 1.1.15.1027 (`v327680`), Unity 2019.4.40f1 IL2CPP, SDK 14.3.0, NVN 55.15 and FMOD. The game loads `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The supplied Echoes of the Eye package carries the license for add-on index 1. The game shows the expansion in the title and displays its first-run notice.

## What it needs

| Setting | Why |
|---|---|
| `depth_mode zero` | The game sets its near depth to zero. The prebuilt pipelines need the same vertex shader entry point as live draws. |
| `mem_reserve 0x160000000`, `mem_alias 0x138000000` | The title fit the provisional 4.375 GiB alias region, but the first expedition reached +4.53 GiB on the Mac. The 4.875 GiB alias region leaves room for the original StandardAllocator request and Unity's later regions. |
| `sys_resource 0x1000000` | The NPDM declares 16 MiB of system resource. The game uses address regions at startup. |

The shared SDK rules reduce the address-region node area and align large regions to 2 MiB, as in [Chained Echoes](../0100C510166F0000/README.md#what-it-needs), which ships the same `sdk` build. There are no rewriter cuts for `main` or `subsdk0`.

## Color grading

The intro and title rendered their star field with a gray background and dark rings around bright stars. A pass dump showed a black HDR scene before `Hidden/Post FX/Uber Shader` sampled the 1024 × 32 grading LUT. The LUT's first two neutral entries were 0.992. The upstream formula maps both to zero. With the original translation, a dark sky pixel was RGB `(59, 59, 59)`.

The shipped Maxwell LUT generator (`1336ea8a75a3b653`) compared its nonnegative channels to zero and took `log2(x)` for black. The upstream Unity formula uses a threshold of `2^-15` and adds `2^-16` to `x / 2` below it. Both constants are half-precision subnormals and are absent from the compiled instructions. A strict Metal recompile and disabling deferred uniforms left the capture unchanged. [Unity's ACES conversion](https://github.com/Unity-Technologies/PostProcessing/blob/v1/PostProcessing/Resources/Shaders/ACES.cginc#L212-L226)

The offline translation restores the upstream low range only when the Maxwell shader payload matches byte for byte. The repaired shader gave zero at both dark LUT entries and RGB `(0, 0, 0)` at the same sky pixel. The brightest captured red channel measured 206 in both renders. The patched bytecode and a separate test that restored the upstream formula in Metal produced identical LUT bytes. The LUT is generated at scene load.

## Frame rate

The title switches to two vsyncs per present and the game runs at 30 fps. The profile defaults to 60 fps through Unity's existing limiter. The rewriter finds `QualitySettings.vSyncCount` and `Application.targetFrameRate` through Unity's internal-call registration, and the host sets the active quality level's count to one and paces presents at the target. This works on builds recognized by the same Unity discovery. [Unity's frame-rate discovery](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table)

On the M6 Mac at 1920 × 1080, the last 300-frame window at the opening campfire measured 33.33 ms median and 33.60 ms p95 with the game's pacing, and 16.67 ms median and 16.78 ms p95 at the 60 fps target. The launcher offers a 30 fps option.

## Mac controls

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app`. Open the app, use J (A) to dismiss notices and start New Expedition, then I (Y) to wake up. To see the Metal HUD, launch with `DM_METAL_HUD=1`.

## Testing

- 2026-11-25, Apple M6, macOS 27.2.1, Xcode 27.2. All 20,355 shaders built for Mac and iOS. The signed Mac app and unsigned IPA passed their package checks.
- The Mac app reached the title, opened a new expedition and passed Wake Up into stationary gameplay. With `DM_ALIAS_SIZE=0x138000000`, the campfire's live regions reached +4.51 GiB with 2.14 GiB of guest memory mapped.
- Stereo CoreAudio logged underruns during startup and zero underruns in the next 16,000 callbacks through the campfire.
- Later areas, ship flight, save/reload, and on-device iOS gameplay remain untested.
