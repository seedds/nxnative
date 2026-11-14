# Trails in the Sky 2nd Chapter (`01007A2027548000`)

Update 1.0.4 (v393216), SDK 21.4.4, NVN. Nihon Falcom's engine, the same as the Trails in the Sky 1st Chapter remake's (no profile yet: issue [#877](https://github.com/seedds/nxnative/issues/877), PRs [#176](https://github.com/seedds/nxnative/pull/176) and [#1002](https://github.com/seedds/nxnative/pull/1002)). Modules `main`, `subsdk0` (the SDK's movie player) and `sdk`. The base game is a cartridge image (v0, 29.75 GB); the RomFS of 1.0.4 is 134,636 files, 24.6 GB. The 30 DLC NSPs on hand hold a licence and no content.

On the Mac (window and HUD, 2026-11-13): the title screen, New Game on Normal, No to the 1st Chapter summary, the opening shot of Grancel, the prologue's cutscenes in Grancel Castle (the guest room, the corridor), then the garden terrace in control ("Look for Cassius"), walking, the save menu (a new save written, then loaded with Continue in a second run), about seven minutes in all. The user, watching the window, judged it good. 60 fps on the title (CPU 16% of a core, 1.1 GB footprint) and 59.2 fps in the garden (CPU 21%, 2.4 GB footprint), p99 frame 25 ms, 4 to 7 of 300 frames dropped. The characters' eyes (the 1st Chapter's white eyes in #176) draw correctly. Sound was on (`DM_AUDIO=1`) and not checked. Not run on the iPhone (see [Open](#open)).

## What it needs

The provisional profile's guesses held: no key needed changing. The tool and host changes it needed are general ([Found on this game](#found-on-this-game)).

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM declares 16 MB of system resource: virtual address memory |
| 5 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x98000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | `main` asks for a 2.02 GB heap (the NSP script's heap check): a 2.125 GB heap region. The alias region is the iPhone layout; the Mac's is 64 GB ([The Mac's alias region is the Switch's 64 GB](../../../docs/reference/memory.md#the-macs-alias-region-is-the-switchs-64-gb)) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 at boot |

No `patches.py`: the rewriter needs no cut on the Mac.

## Memory

- **Current:** at boot the engine's allocator takes one address region of 11.27 GB, `0x2d1400000` = ten times `0x48200000`, returning to `main+0x6a1dcc` (the NSP script's address region check, `logs/regions.tsv`), at alias +1.00 GB. It maps 0.40 GB of it in the first 15 s, and the footprint was 2.4 GB in the garden. The 1st Chapter remake's allocator reserves the same ten times the same capacity (#176: `main+0x5746f4` in its build).
- The Mac's 64 GB alias region holds the region. An iPhone's (2.38 GB here, 6 GB of address space in all without the extended addressing entitlement) cannot ([Open](#open)).

## Shaders

- **Current:** 11,340 keys: 11,322 GLSLC compiler outputs in `asset/nx/shader/*.nvn` and 18 in `main`. The split reads the 24.6 GB RomFS in about 3.5 minutes; the Mac build of all of them takes about 16 minutes, and all of them translate.
- `main`'s compute shader at `main+0x7774a8` carries GLSLC's assembly listing after its control section; the split now reads it by its section header ([GLSLC outputs read through their section headers](../../../docs/reference/shaders.md#glslc-outputs-read-through-their-section-headers)). Before, its dispatch in the garden had no compute shader, and the build had one translation failure (`IndexOutOfRangeException`), which went away with the fix (most likely that shader's misread code area; not checked).
- A compute shader asks for 45,056 bytes of threadgroup memory (`37727c65c9440244`), past Metal's 32 KB: served from a device buffer, as in the 1st Chapter.

## Mac routes

- Fresh save to control (`DM_CTL`, a press of 0.1 to 0.15 s): wait about 35 s for the title; the cursor was on Load Game, `LUp` put it on Continue and `LDown` then on New Game; A, A on Normal, A on Yes, `LDown` and A for No to the summary, then A about every 2.5 s through the cutscenes, about 30 presses, until the garden terrace.
- From a save: A on Continue at the title loads it; a tips screen shows during the load.
- Controls: the left stick walks; X opens the menu with Save.

## Found on this game

General changes first needed here; each applies to every game:

- **A one-stage GLSLC output read by its section header** (`glslc_shaders` in `shader_split.py`): [GLSLC outputs read through their section headers](../../../docs/reference/shaders.md#glslc-outputs-read-through-their-section-headers).
- **`nvnTextureBuilderGetDepthStencilMode`** (the mode `nvnTextureBuilderSetDepthStencilMode` stored) and **`nvnCommandBufferSetControlMemoryCallbackEnabled`** (like the command memory switch, it changes nothing: the host calls the game's callback only where it would otherwise lend memory of its own) in `hle_nvn.c`: each was a `[RED] NVN unimplemented function called` line at boot and in the prologue.
- **Integer outputs told by the pipeline's targets** (resolved): `[RED] shader … writes colour target 1, an integer format, as a float: its integer entry point returns outputs 0x6 as integers, which is not this pipeline's set (integer targets for outputs 0xe, targets for outputs 0xf)`, once a run from a different shader each time (`10cd95a2a310ea8c` in the castle's cutscenes, `979fd4d4d4bd7615` loading the garden, then `b09f3ac96129e558`, `96ec090803707686` and the deferred outline pass `aa682f8e32981529` as each rule was added). The G-buffer is float at colour 0 and integer at colours 1 to 3, and the mixed entry point chosen from the shader's code (`main0_m`) left out an output that goes to an integer target, so those pipelines drew with `main0` and their integer targets took undefined values; nothing wrong was seen on screen. Four rules in turn (a uniform word written straight into an output, outputs of nothing but zeros, a register value that every branch overwrites, texels read with `read()`) each moved the red line to the next shader. Now `main0_u` returns each colour as a float or as integer bits by a function constant, and the host specializes it with the pipeline's integer targets: [Integer outputs told by the pipeline's targets](../../../docs/reference/shaders.md#integer-outputs-told-by-the-pipelines-targets). The 1st Chapter met the same red line (#1002).

## Open

- Not run on the iPhone. The 11.27 GB address region does not fit an iPhone's alias region, so the game would fail at boot there. It needs either the allocator's reservation cut down for this `main` build (the 1st Chapter's way, in #176: ten times to three times), which is bound to the build, or a general mechanism for regions taken with `nn::os::AllocateAddressRegion` (the host's StandardAllocator cap does not reach them, [StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space)), which would hold only if the allocator places nothing by fixed offsets in the region.
- Only update 1.0.4 is registered; the base game alone (v0) is not checked. No smoke route yet.
