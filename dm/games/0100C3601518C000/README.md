# Sid Meier's Civilization VII (`0100C3601518C000`)

Version 1.5.0.1 (`v1638400`), SDK 19.3.11, Firaxis's engine with Coherent Gameface / V8 and Wwise, on NVN. Modules `main` (`5d9401301ab2bb4917bafd939e5b3dd8`) and `sdk` (`df7a3960c74c3a4803f826c587153252`); the registered `MediaDecoders.NN.nro` is rewritten and loaded through `nn::ro`.

**Mac gameplay tested, 2026-11-15:** first-run display options, main menu, leader/civilization selection, world generation, unit selection and the founder's settlement action (London, turn 1), then Quick Save. Audio plays with no underruns. The gameplay check stayed under ten minutes. iOS is blocked by address space, not yet supported; see [Memory](#memory).

## What it needs

| Setting | Why |
|---|---|
| `sys_resource 0x200000` | the NPDM declares 2 MB of system resource, enabling the SDK's virtual address memory |
| `app_pool 0xCD500000` | hardware application-pool budget; the game requests a 3.01 GB heap |
| `mem_reserve 0x300000000`, `mem_alias 0x218000000`, `mem_stack 0x10000000` | 3.125 GB heap region, 256 MB stacks, 8.375 GB alias region; the Mac automatically uses a 64 GB alias region with the same heap/stack partition |
| `depth_mode zero` | the game calls `nvnDeviceSetDepthMode` with near = 0 |

No game-specific code patches, host branches or new profile switches. The existing SDK matcher reduces the address-region node area; the existing general odd-stack-frame rule handles this build's four hand-written SVC blocks.

## Rendering

`ShaderAutoGen_Linux_NVN_FinalRelease.bs` holds 801 single-stage CIVBSF records: 357 vertex, 343 pixel and 101 compute records, with duplicates. The split finds 481 distinct shaders (127 vertex, 254 pixel, 100 compute). All 481 compile on the Mac, with zero undecoded bindings and zero driver-bank reads the host cannot fill. The existing pixel-input cover supplies vertex padding; the library's effect-to-stage pairing is not decoded.

The prewarm list and seen-shader list cover first-run menus, new-game setup and the first settlement. The shader map was compared with the runtime: no shader was missing on that route. All 481 shaders are created during startup. The two measured dual-source programs are built with their existing dual-source entry points.

Final Mac app build: 7.9 GB, chain `cd731f39944d`, shader inputs `20383d074091`. Its rebuilt prewarm archive contains 77 render and 20 compute pipelines, with zero failures and no dual-source warning. The graphics suite, focused shader-reader tests, profile/documentation checks and packing-script tests pass; Python lint has no new findings. No `regress.py` game run: reach is narrow and clear (a new container-magic path and the offline prewarm stamp lookup), with no host or chain change.

On the Apple M6, the settled map held about 60 fps: frame time p50 16.66 ms, p95 17.04 ms, GPU p50 5.57 ms over a 300-frame window. Resident memory reached 3,330 MB, Metal allocations 2,828 MB, then stayed near that level in this short test. These are absolute measurements, not a performance comparison with another port.

## Memory

The game sets a 3.01 GB main heap. Its Coherent UI also needs V8's pointer-compression cage: `IsolateAllocator::InitializeOncePerProcess` (`main+0x2d05a50`) supplies a 4 GB reservation size and 4 GB alignment. `VirtualMemoryCage::InitReservation` (`+0x305d8b8`) first reserves 8 GB to find an aligned interior address, frees it, and retries 4 GB with that address as a hint. Coherent's `v8::base::OS::Allocate` (`+0x2a69af8`) discards the hint; AppUI's allocator (`+0x5f4964`) uses the SDK's address selection. Runtime probes show repeated 8 GB / 4 GB attempts, followed by an oversized reservation retained around the aligned cage.

The automatic region check measured an 8 GB region at alias +0.06 GB, opened through region +2.76 GB with about 6 MB mapped in that region. A 5 GB overall block with a 1.375 GB alias region cannot hold it. The profile records a 12 GB layout that holds the observed request; this is above the iPhone budget without extended virtual addressing. The normal Mac alias region fits it.

**Open, iOS:** correctly aligned Coherent/V8 allocation can avoid the doubled reservation, but even the 4 GB cage plus the main heap exceeds the current iPhone budget. A compact cage or allocator replacement must preserve V8's pointer compression, bounded allocator and GC assumptions. Neither a blind size clamp nor a generic `AllocateAddressRegion` that returns less memory is valid. No such workaround has been added.

## Mac route

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` (or `--platform mac-app` for a standalone app). The work folder has `run.py`: `boot <log> <seconds>`, `step <shot> <buttons...>`, `ctl <line...>` and `stop`; it uses a dedicated `civ7host`, window, sound and Metal HUD. The build's `run/save` holds the tested quick save.

From the main menu: A on New Game; Right, A selects Ada Lovelace; A opens Great Britain's details, A selects it; A continues the default small-map setup. On Overview, Down, A chooses Launch Game. After generation, A begins; R skips the introductory help cards. On the map, A selects the starting hex, A opens the founder's actions, Right, A founds London. Plus, Down, A quick-saves. Touch input was not needed for this route.

The supplied folder has 42 licence-only DLC packages; the game displayed owned content in its pause menu. The base game and other updates have not been tested.

## Found on this game

- [Firaxis CIVBSF shader libraries](../../../docs/reference/shaders.md#firaxis-civbsf-shader-libraries).
- [Mac prewarm pipelines reused after a packed shader library changes](../../../docs/reference/shaders.md#mac-prewarm-pipelines-reused-after-a-packed-shader-library-changes): stamp lookup now follows the packed library.
- [Stack frames of an odd size](../../../docs/reference/rewriter.md#stack-frames-of-an-odd-size-sub-sp-sp-imm): existing general rule, no new cut in this port.
- [A written secondary blend source](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending): two measured shaders (`064c4ce6df409438`, `f2755f3bfebde977`), originally in `dual_want.tsv`, now handled by `main0_u` in `<key>_bx`.

## Open

- iOS support: the V8 cage and main heap exceed the address-space budget described above; no working IPA claimed.
- The run logs a failed initial upload of a multisample Depth24 texture (`format 0x33`, target 5). Both GPU and CPU deswizzle reject multisample geometry; the Metal substitute also requires depth conversion. The observed map renders, but the upload's relevance and later shadow/rendering coverage remain unverified; no blanket skip was added.
- Quick Save was written and appeared in Load Game with the correct turn and civilization. A reload was started, but its return to the map was not verified before the ten-minute gameplay-testing budget ended.
- Later turns, age transitions, multiplayer, long-session memory stability, and full movie coverage have not been tested.
