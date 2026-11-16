# Sid Meier's Civilization VI (`010044500C182000`)

Control version 1.2.19 (`v1638400`; the game displays 1.2.18, build 376452), SDK 8.2.1, Firaxis's engine with Havok and Wwise, on NVN. Modules `main` (`56dc86f30efa78d01071458d31be8d25`), `subsdk0` (`c8a3cc0b1d1858330a881c0c2fc87940`, GLSLC) and `sdk` (`5fd4f531c1195e9e5510c9e73f22cd39`). The base, Expansion 1 and Expansion 2 game-core NROs are registered and loaded through `nn::ro`.

**Mac tested, 2026-11-15/16:** startup, readable menus, new-game setup/world generation and the turn-one Ethiopia map. The player's turn-one autosave reloads successfully. The reported magenta terrain and white trees are resolved; the same save now has green terrain, coloured trees/units and the settler's green/red/grey water-availability overlay. Gameplay coverage is limited to the opening map; diagnostic reloads were used to trace rendering. iOS device testing remains open.

## What it needs

| Setting | Why |
|---|---|
| `sys_resource 0` | the NPDM declares no system resource, so the SDK does not use virtual address memory |
| `app_pool 0xCD500000` | application-pool budget; the SDK requests a `0xc9400000`-byte heap |
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000` | 4 GiB layout with a 3.5 GiB heap partition, 256 MiB alias region and 256 MiB stacks |
| [`dual_want.tsv`](dual_want.tsv) | three observed pixel shaders write the secondary blend source; enables existing dual-source entries |

No game-code patches, fixed-address gates, host game branches or new profile switches. Shader-container support is bound to the CIVBSF format revision; the text and colour fixes implement general descriptor semantics.

## Rendering and shaders

`Base/Binaries/Nx/ShaderAutoGen_Nx_DX11_FinalRelease.bs` is a 23,498,397-byte CIVBSF revision `0x1118` library. Its 539 stage records yield 488 distinct shaders: 134 vertex, 320 pixel and 34 compute. All 488 compile for Mac and iOS, with no undecoded binding or unfillable driver-bank read. This older revision uses 0x48-byte records and NVBC-wrapped GLSLC payloads; [the reader](../../../docs/reference/shaders.md#firaxis-civbsf-shader-libraries) validates that representation without a title/build gate.

- **Resolved, missing text:** buffer-texture handles held the sampler word first and texture word second. Retaining and reading both words restores glyph vertices. See [A buffer texture with a two-word handle](../../../docs/reference/shaders.md#a-buffer-texture-with-a-two-word-handle).
- **Resolved, magenta map:** compute shader `06932ac941e93119` fills a 32³ colour LUT, which `65381f87fa4f1382` samples in the final colour pass. Its recorded image bind inherited the texture pool at execution, but the host resolved index 360 during recording and wrote a dummy image. The corrected trace writes the registered LUT, host ID 30824, shared by the sampled view at 30823. See [Descriptor handles use the execution pools](../../../docs/reference/shaders.md#descriptor-handles-use-the-execution-pools). Disabling deferred/lazy uniform updates did not change the failure; no such switch is required.
- **Resolved, four compute shaders:** all 382 bindless buffer-texture accesses in `2275dfc526ca0835` (4), `9a773e7a65669a0a` (52), `a702a5e2a2dbc2d1` (205) and `ad8fb4308f8f1f70` (121) translate without deletion. Their handle ORs a word at offset 0 of uniform slot 12 with a word at offset 0, 16, 24, 32 or 40 of slot 13, loaded through the compute uniform-address table. See [Compute texture handles loaded through uniform addresses](../../../docs/reference/shaders.md#compute-texture-handles-loaded-through-uniform-addresses). None was dispatched on the traced opening-map route; their later gameplay use remains untested.
- Two tiny device-initialization shaders, `344200c2e0c5ae54` and `76928576cf77ad1e`, are absent from the extracted library. They appear before the main rendering device; their purpose remains open. No map shader was missing on the observed route.

The seen-shader and prewarm lists cover startup, menus, game setup and the first map. Later turns and effects can require additional pipelines.

The compute-handle fix changes 31 of the 34 compute shaders: the four restored programs and 27 whose known uniform handles previously took the general texture-pool lookup. The other three compute programs retain identical SPIR-V. Both native translator architectures produce identical SPIR-V for the four restored shaders. All 488 shaders rebuild for Mac and iOS with no deleted access, undecoded binding or unfillable driver-bank read.

## Performance and validation

On Apple M6, the corrected turn-one map held about 60 fps: one 300-frame window reported frame-time p50 16.67 ms / p95 16.73 ms and GPU p50 5.88 ms. Process footprint reached about 3.85 GiB in the diagnostic run with a full pass dump. This is an absolute observation, not an isolated before/after performance comparison.

The changes fix rendering rather than target performance. Resolving explicit descriptors at use adds small integer operations to binding/dependency checks and removes recording-time relocation; no material frame-rate change is expected. The shader-reader change affects builds only. Binding of Isaac and Xenoblade Chronicles 2 quick and full regressions pass, chosen to cover existing NVN paths with established automated routes; the broader host reach warrants two games. Both retain their full shader census and have no new RED or interface failure. Full prebuilds made 18 and 12,370 bases respectively, with zero failures. Isaac's full baseline was marked busy, so no performance improvement is claimed from that comparison. After rebasing onto `94a09447`, both quick routes pass again on chain `30f68cc57128`, with unchanged shader counts and no new RED or interface failure.

The full graphics suite passed before rebasing; focused descriptor, buffer-handle, recorder, reflection and capture/replay checks pass after rebasing. The GPU fixtures pass under Metal validation: 23 descriptor-pool checks and 20 two-word buffer-handle checks. Before rebasing, all 1,685 tool tests passed (16 skipped), using the installed .NET runtime with major-version roll-forward for extraction fixtures. After rebasing, 1,710 tool tests ran with two failures: the environment-switch documentation check fails identically on unchanged upstream `94a09447` (missing `DM_OBSOLETE` metadata and mentions of removed switches), and `test_triage_report.test_main_load` failed its captured-output prefix assertion in the full suite but passed in isolation on both trees. No unrelated test fix is included. Python lint and LLVM 23.1.4 C/Objective-C lint have no new findings.

**Compute follow-up, 2026-11-16:** the nine synthetic handle tests pass, including a translated GPU fetch under Metal validation, both word orders, masked handles, descriptor boundaries, dynamic and oversized offsets, and graphics-stage isolation. The production two-word binding fixture passes all 20 GPU checks. The shared shader/reflection suite has 482 passes and 10 skips; its game-setting-dependent pair test passes when rerun with `DM_GAME` set. The 42 documentation/ABI/chain checks pass, and Python lint has no new findings. Full Binding of Isaac and Xenoblade Chronicles 2 regressions pass against `7b67bfe0`: all 19 and 15,910 shaders respectively build, no MSL changes, no new interface/RED failure, and all 18 and 12,370 pipeline bases build. These two available automated NVN routes cover the uncertain reach of a shared translator change.

A 100-second Mac save reload on the new chain reaches the turn-one map with the same green terrain, coloured trees/units and settler overlay. Its last 300 frames have frame-time p50 16.67 ms / p95 16.71 ms and GPU p50 6.01 ms on Apple M6; this is an observation, not an isolated performance comparison. The four restored programs are still outside that opening-map route. Startup retains the previously recorded `chdir64` lookup and two missing device-initialization programs. No new rendering failure appeared.

After rebasing onto `75758b94`, both games' quick regressions pass again with unchanged shader counts and MSL and no new interface or RED failure. The nine shader fixtures, 20 production GPU binding checks and 42 documentation/ABI/chain checks pass again. Both app hosts were rebuilt with the rebased code; the shader chain and inputs are unchanged.

Final artifacts: `Sid Meier's Civilization VI.app` (24.3 GB, ad-hoc signature verified) and `SidMeiersCivilizationVI.ipa` (16.3 GB, unsigned). Both use chain `4d84aa6c8a2d`, profile shader inputs `370a01a02359`. The Mac prewarm built 69 render and four compute pipelines with zero failures. The standalone launcher was not separately played. The IPA has not been installed or tested on a device.

## Mac route

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app` for the standalone Mac app, `--platform mac` for the development host, or the default for an unsigned IPA. The game's work folder contains a dedicated `civ6host` and `run.py` diagnostic helper; extracted assets, logs and saves remain outside the repository.

The tested save is `run/save/appdataparent/Sid Meier's Civilization VI/Saves/Single/auto/AutoSave_0001.Sav`. After startup, A enters Single Player and A resumes the game. When the restored leader screen shows Continue Game, A enters the map. Timing depends on the legal screen and whether the introductory video is enabled. The local test setting `PlayIntroVideo 0` skips that video; the port does not modify it automatically.

The supplied folder has 18 DLC packages, two of them licence-only. Other game builds, later turns, battles, diplomacy, multiplayer, long-session memory stability, physical controllers and iOS gameplay have not been validated.

## Found on this game

- [Firaxis CIVBSF shader libraries](../../../docs/reference/shaders.md#firaxis-civbsf-shader-libraries): older NVBC-wrapped revision.
- [A buffer texture with a two-word handle](../../../docs/reference/shaders.md#a-buffer-texture-with-a-two-word-handle): missing glyph geometry.
- [Descriptor handles use the execution pools](../../../docs/reference/shaders.md#descriptor-handles-use-the-execution-pools): unwritten colour LUT and corrupted map colours.
- [Compute texture handles loaded through uniform addresses](../../../docs/reference/shaders.md#compute-texture-handles-loaded-through-uniform-addresses): 382 restored buffer fetches in four compute shaders.
