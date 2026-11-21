# LOLLIPOP CHAINSAW RePOP (`0100DD301A686000`)

Update 1.21.1 (`v1245184`) over the base game. Unreal Engine 5.6.0
(`SDK MW+EpicGames+UnrealEngine-5_6_0` in `main`), Nintendo SDK 20.5.17,
NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

**Current Mac limitation: the captured world is black behind the visible HUD.**
Startup reaches the frame loop with the automatic Unreal host allocator, but
current scene rendering and the school-gate endpoint are not visually validated.
The cause is unresolved; the existing captures do not locate the first bad
scene attachment or distinguish it from a final-composite/capture problem.

**Historical manual Mac gameplay, confirmed by the user on 2026-10-31,**
covered the first combat wave and parking-lot transition. That result and the
measurements below predate the current main; they do not establish current
playability. Longer play, later-checkpoint persistence, listening quality and
iOS device behavior remain unverified.

## Memory

- **Current platform-limit workaround:** the game's
  FMallocBinned3 constructor reserves 38 GiB of address space (76 pools of
  512 MiB), beyond the iPhone's available virtual address space. The
  engine-wide discovery in `ue_malloc.py` and `hle_uemalloc.c` use the existing
  host allocator replacement, a workaround for that platform limit. The former
  fixed constructor cut and host row are superseded by the discovery added in
  `51904e70`. On the untouched 1.21.1 module, the finder returns the verified
  constructor call and vtable below, with the name getter at slot 26.
- `nninitStartup` is a branch at `main+0x35107c0` to `+0x13370`. Its call at
  `+0x137f0` constructs the static allocator at `+0x7028210`; the constructor
  is `+0xae550`, and the verified 32-slot vtable is `+0x63b10b8`. These offsets
  apply only to main build `89f76888939adcdf69f8b0e647d84cad`.
- The FMalloc interface matches `UE5_6`: Malloc / Realloc / Free in slots
  6 / 8 / 10, MallocZeroed / TryMallocZeroed in 11 / 12, QuantizeSize in 13,
  GetAllocationSize in 14. TLS allocation and release call through that
  vtable; no direct calls to the replaced allocation methods were found.
- **Current automatic sizing:** the engine-wide region finder added in
  `2006d5ed` finds the size store at `main+0x137cc` (x1), immediately before
  the reservation call at `+0x137d0` in `nninitStartup`'s `+0x13370`.
  `OBS_UE_REGION` sizes it from available alias space; the fixed
  `game_args -SwitchVirtualAddressSpaceSize=4064` is removed. In this profile's
  constrained 5 GiB guest block (`mem_reserve 0x140000000`,
  `mem_alias 0x118000000`, `mem_stack 0x10000000`), the initial region
  budget is 4160 MiB (4.0625 GiB). This is the iOS-sized configuration. The
  Mac now uses the Switch's 64 GiB alias region by default, so the engine's
  62 GiB request fits unchanged. A normal Mac boot does not validate the
  constrained iOS region budget.
- **Historical gameplay:** the tested argument requested 4064 MiB, 96 MiB
  less than the constrained initial budget. That Mac route reached alias +2.76 GiB
  with about 1.50 GiB mapped; the host allocator held about 0.50 GiB. Those
  measurements do not validate the new automatic region sizing or longer play.
- `sys_resource 0x5000000` comes from the NPDM's 80 MiB system resource.
  SDK build `0023a1b06130cd795f7bc4613772af0a` already has the shared
  62 MiB address-region node-area cut.

## Files and shaders

- The merged RomFS has 111 files, 6.51 GiB: one pak, two IoStore containers
  (`.utoc` / `.ucas` pairs), and 99 Bink 2 (`.bk2`) movies.
- IoStore shader extraction yields 10,082 entries (8,166 game and 1,916
  global). The split deduplicates them into 9,828 shaders: 908 vertex,
  7,712 pixel and 1,208 compute shaders. The 1,816 vertex/pixel pairs cover
  the separable programs' pixel inputs.
- `tex_dyn_slots on` uses the existing dynamic texture slot binding
  for 12 vertex shaders. All 89 distinct dynamic buffer reads
  found in the game's shaders take handles only from c2 driver tables: texture
  bytes `0x168`, `0x180`, `0x188`, `0x190`, with sampler words `0x568` or
  `0x578`. Their buffer index is `min(handle & 0xfffff, 31)`; no game-memory
  value supplies a handle. The declared dynamic sampler arrays are unused.
  The build summary counts these dynamic bindings as unresolved; the enabled
  host path resolves them at draw time.
- `depth_mode zero` seeds pipeline prebuild with the zero-to-one depth range
  observed at startup. The runtime follows the game's depth-mode calls.
- The prewarm seed records 550 shaders, 463 render pipelines and 53 compute
  pipelines from startup through the first combat wave and parking lot.
- `pad_layout full` exposes both sticks for movement and camera control.
- **Resolved on main (`d20e9e7e`):** global `ATOM.CAS` was silently omitted in three compute
  shaders (`56e279e8fac3fa1c`, `b63078f53df8db95`, `e918336692525691`). The
  last is `/Engine/Private/GPUFeedbackCompaction.usf:BuildFeedbackHashTableCS`
  (global archive entry 324), recorded in an actual compute dispatch during
  prologue gameplay. Its instruction at code offset `0x208`
  (`0xeef1000000c70201`) compares the bucket with R12, replaces it with R13
  on a match, and returns the old value in R1.
- **Open:** the translator still reports `Sam`, `Ram` and `PrmtI` in shader
  paths whose correctness is not established by the short route. A successful
  Metal compile does not validate those instructions or unvisited paths.

## Current limitations and historical measurements

Current visual validation is blocked by the HUD-only captures described above,
even though the game continues submitting draws without a reported runtime
error. No current gameplay or performance pass is established. A selected-frame
pass/attachment dump is needed to locate the first black world output; it has
not been captured. Unvisited shader paths and later stages remain unverified.

The following Apple M2 Max short-route measurements are historical, on chain `d54f11c2255c`
and the former fixed 4064 MiB region. They are not a controlled performance
comparison or current-head validation.

| Prologue observation | Historical measurement |
|---|---:|
| Typical frame interval | 16.7 ms (about 60 fps) |
| 95-second smoke frame-time p99 | 18.03 ms |
| Resident memory | 4.0–4.2 GiB |
| Metal allocations | 2.6–2.8 GiB |
| Prewarm load, 1,066 entries | about 1.2 s |

Historical manual testing covered startup, the title/menu flow, gate
interactions, first combat and the parking-lot transition. Later stages, long sessions, later-save persistence,
audio listening quality and iOS device memory/performance remain open.
The older [`validated.tsv`](validated.tsv) stamp applies only to its recorded
chain. The current black-world blocker prevents a new green stamp, and a short
smoke route cannot extend the historical manual route coverage.

## Mac route

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <keys> --platform mac`.
The script prints the environment and command for the host. Keep saves and
captures in `<NSP folder>/0100DD301A686000-work/`.

- From a new save, `DM_KEYS="12:A,17:A,22:A,27:A,60:Plus,62:A"` skips the
  startup movie, selects Game Start and Original Mode, and skips the prologue
  cutscene. The recorded route's intended endpoint is the school gate.
  [`smoke.tsv`](smoke.tsv) captures frames at 75 and 90 seconds; its frame-count
  predicate alone cannot establish visible gameplay, as the current HUD-only
  captures demonstrate.
- Move forward with `LUp~2`, then press X and Y at the gate's prompts. After
  its short cutscene, move forward again and attack with X/Y. The right stick
  turns the camera; Plus opens the pause menu, and B closes its pages.
- The manual test defeated the first wave and walked into the parking lot.
  The bundled prewarm list covers that route. It does not establish full-game
  compatibility.

## Found on this game

- [The Mac's alias region is the Switch's 64 GB](../../../docs/reference/memory.md#the-macs-alias-region-is-the-switchs-64-gb).
- [Unreal's engine address region and its options](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options).
- [Unreal's FMallocBinned3 replaced by the host allocator](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator).
- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area).
- [Texture slots picked at run time](../../../docs/reference/shaders.md#texture-slots-picked-at-run-time).
- [Maxwell ATOM.CAS](../../../docs/reference/shaders.md#maxwell-atomcas).
