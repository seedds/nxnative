# LOLLIPOP CHAINSAW RePOP (`0100DD301A686000`)

Update 1.21.1 (`v1245184`) over the base game. Unreal Engine 5.6.0
(`SDK MW+EpicGames+UnrealEngine-5_6_0` in `main`), Nintendo SDK 20.5.17,
NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

**Current:** playable on the Mac through the prologue's first combat wave and
the parking-lot transition. The unsigned iOS package builds and passes archive
verification; device validation remains open.

## Memory

- **Current platform-limit workaround:** the game's
  FMallocBinned3 constructor reserves 38 GiB of address space (76 pools of
  512 MiB), beyond the iPhone's available virtual address space. The
  engine-wide discovery in `ue_malloc.py` and `hle_uemalloc.c` use the existing
  host allocator replacement, a workaround for that platform limit. The former
  fixed constructor cut and host row are superseded by the discovery added in
  `51904e70`; checking discovery on this build remains open until its extracted
  modules are available again. See [Unreal's FMallocBinned3 replacement](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator).
- `nninitStartup` is a branch at `main+0x35107c0` to `+0x13370`. Its call at
  `+0x137f0` constructs the static allocator at `+0x7028210`; the constructor
  is `+0xae550`, and the verified 32-slot vtable is `+0x63b10b8`. These offsets
  apply only to main build `89f76888939adcdf69f8b0e647d84cad`.
- The FMalloc interface matches `UE5_6`: Malloc / Realloc / Free in slots
  6 / 8 / 10, MallocZeroed / TryMallocZeroed in 11 / 12, QuantizeSize in 13,
  GetAllocationSize in 14. TLS allocation and release call through that
  vtable; no direct calls to the replaced allocation methods were found.
- `game_args -SwitchVirtualAddressSpaceSize=4064` requests a 4064 MiB engine
  region inside a 5 GiB guest block (`mem_reserve 0x140000000`,
  `mem_alias 0x118000000`, `mem_stack 0x10000000`). The parser accepts MiB up
  to 4095 and allocates the region when the option is supplied. The tested Mac route reached alias +2.76 GiB with about 1.50 GiB
  mapped; the host allocator held about 0.50 GiB. Longer sessions are untested.
- `sys_resource 0x5000000` comes from the NPDM's 80 MiB system resource.
  SDK build `0023a1b06130cd795f7bc4613772af0a` already has the shared
  [62 MiB address-region node-area cut](../../../docs/reference/memory.md#the-sdks-address-region-node-area).

## Files and shaders

- The merged RomFS has 111 files, 6.51 GiB: one pak, two IoStore containers
  (`.utoc` / `.ucas` pairs), and 99 Bink 2 (`.bk2`) movies.
- IoStore shader extraction yields 10,082 entries (8,166 game and 1,916
  global). The split deduplicates them into 9,828 shaders: 908 vertex,
  7,712 pixel and 1,208 compute shaders. The 1,816 vertex/pixel pairs cover
  the separable programs' pixel inputs.
- All 9,828 shaders compiled for Mac and iOS on chain `d54f11c2255c`, including 166
  shadow-input, 7,064 alpha-test and 868 pulling variants; no driver-constant
  read was classified as impossible to fill.
- `tex_dyn_slots on` uses the existing [dynamic texture slot binding](../../../docs/reference/shaders.md#texture-slots-picked-at-run-time)
  for 12 vertex shaders. All 89 distinct dynamic buffer reads (277 including
  generated entry copies) take handles only from c2 driver tables: texture
  bytes `0x168`, `0x180`, `0x188`, `0x190`, with sampler words `0x568` or
  `0x578`. Their buffer index is `min(handle & 0xfffff, 31)`; no game-memory
  value supplies a handle. The declared dynamic sampler arrays are unused.
  The translator's unresolved-slot summary still counts these arrays; the
  enabled host path resolves them at draw time.
- `depth_mode zero` seeds pipeline prebuild with the zero-to-one depth range
  observed at startup. The runtime follows the game's depth-mode calls.
- The prewarm seed records 550 shaders, 463 render pipelines and 53 compute
  pipelines from startup through the first combat wave and parking lot. A fresh
  launch loaded all 1,066 entries with no stale shader keys in about 1.2 seconds.
- `pad_layout full` exposes both sticks for movement and camera control.
- **Resolved:** global `ATOM.CAS` was silently omitted in three compute
  shaders (`56e279e8fac3fa1c`, `b63078f53df8db95`, `e918336692525691`). The
  last is `/Engine/Private/GPUFeedbackCompaction.usf:BuildFeedbackHashTableCS`
  (global archive entry 324), recorded in an actual compute dispatch during
  prologue gameplay. Its instruction at code offset `0x208`
  (`0xeef1000000c70201`) compares the bucket with R12, replaces it with R13
  on a match, and returns the old value in R1. See the general
  [global compare-and-swap fix](../../../docs/reference/shaders.md#global-compare-and-swap-omitted-from-translated-shaders).
- **Open:** translation still reports `Sam`, `Ram` and `PrmtI` in shaders
  not recorded in the tested route. Compiling the complete library does not
  establish that those unvisited paths behave correctly.

## Testing

- 2026-10-29: the NSP survey and merged extraction passed for 1.21.1. The
  module build IDs and allocator registration were verified from the binary.
- The 15-second address-region check reached the frame loop with the host
  allocator active. Its 4064 MiB region starts at alias +64 MiB; 0.72 GiB
  was mapped and its opened range reached +1.84 GiB. The check uses a larger
  instrumented address space; this does not establish compact-profile gameplay.
- Mac gameplay checked on an Apple M2 Max (`Mac14,6`), macOS 26.6, Xcode
  27.0: startup movie, title and menus, Original Mode, the prologue cutscene,
  gate interactions, movement and camera, the first zombie wave (score 6,500),
  and the parking-lot transition. No `[RED]` or crash lines occurred.
- The observed gameplay intervals were near 60 fps (frame-time p50 about
  16.7 ms). Resident memory settled around 4.0-4.2 GiB and Metal allocations
  around 2.6-2.8 GiB during this short route. First-use pipeline work and
  screenshot requests caused occasional longer frames; this was a functional
  check, not a controlled performance benchmark.
- The direct audio output produced non-silent 16-bit PCM, with no device
  callback underruns reported during the run. Listening quality is unverified.
- A fresh process with the existing `lollipop` save reached the prologue again,
  with no error lines. This checks save creation and acceptance on relaunch;
  persistence of a later checkpoint has not been validated.
- The corrected chain's 95-second Mac smoke route reached 5,388 frames with
  zero `[RED]` kinds, 59.6 fps and 18.03 ms frame-time p99; its gate screenshots
  match the expected scene. The comparison against the profile's pre-fix
  baseline changed only the three CAS shaders, with no new shader or interface
  failures. See [`validated.tsv`](validated.tsv) for the recorded chain.
- A further four-minute functional check on that chain repeated the gate
  interactions, the grab-escape prompt and combat attacks (score 1,000), with
  no error or crash lines. The earlier test covered the rest of the first wave
  and parking-lot transition.
- After rebasing onto the SDK patch-binding update, all three rewritten module
  binaries matched the tested images byte for byte. The full smoke comparison
  above uses the rebased hosts and profile.
- The unsigned 1.21.1 IPA is 6.81 GiB and contains all
  9,828 base iOS shader libraries and the
  merged RomFS. ZIP CRC verification passed. It has an arm64 executable, no
  provisioning profile or developer identity, and the standard ad-hoc
  entitlement carrier for the increased-memory request. It has not been
  installed or tested on an iOS device.
- The later Portal 2 integration changed the generated gallery and added an
  audio-input notification handler upstream. RePOP's profile, rewritten modules
  and shaders are unchanged; both executables were refreshed and host and
  documentation/patch tests passed. The gameplay measurements above precede
  that final integration.
- **Current validation limit:** rebasing onto `85578fc9` incorporated upstream
  border-colour and logic-operation shader changes, producing chain
  `572d66410ce2`. The Mac host builds; the synthetic CAS, border-colour,
  logic-operation and feedback GPU tests pass. The full-game shader rebuild,
  gameplay regression and IPA rebuild have not been repeated: the previous
  extracted builds and shader caches are no longer available locally. The
  gameplay, IPA and `validated.tsv` results above remain for chain `d54f11c2255c`.
- **Open:** longer play sessions, later stages, later-checkpoint persistence,
  and iOS device performance/memory. The IPA requires iOS 26 or later.

## Mac route

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <keys> --platform mac`.
The script prints the environment and command for the host. Keep saves and
captures in `<NSP folder>/0100DD301A686000-work/`.

- From a new save, `DM_KEYS="12:A,17:A,22:A,27:A,60:Plus,62:A"` skips the
  startup movie, selects Game Start and Original Mode, and skips the prologue
  cutscene. The character stands before the school gate. [`smoke.tsv`](smoke.tsv)
  uses this route and captures idle frames at 75 and 90 seconds.
- Move forward with `LUp~2`, then press X and Y at the gate's prompts. After
  its short cutscene, move forward again and attack with X/Y. The right stick
  turns the camera; Plus opens the pause menu, and B closes its pages.
- The manual test defeated the first wave and walked into the parking lot.
  The bundled prewarm list covers that route. It does not establish full-game
  compatibility.

## Found on this game

- [Unreal's engine address region and its options](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options).
- [Unreal's FMallocBinned3 replaced by the host allocator](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator).
- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area).
- [Texture slots picked at run time](../../../docs/reference/shaders.md#texture-slots-picked-at-run-time).
- [Global compare-and-swap omitted from translated shaders](../../../docs/reference/shaders.md#global-compare-and-swap-omitted-from-translated-shaders).
