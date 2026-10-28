# Pokémon Scarlet (`0100A3D008C5C000`)

Base game 1.0.0 (v0, SDK 13.3.5) and update 4.0.0 (v786432, SDK 13.3.13),
Game Freak's Trinity engine on NVN.
Playable on the Mac: language and character selection, the academy opening,
the native Bink movie, getting up in the bedroom, walking downstairs and
through the living room and kitchen, camera control, dialogue and save/reload.
The unsigned 1.0.0 IPA is built (~7.0 GB); iPhone testing is pending.
4.0.0 is the packed version when its update is in the NSP folder; it runs on
the Mac through the same new-game route to the bedroom
([4.0.0 route](#400-route)). Build 1.0.0 from such a folder with
`scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> -p v0`.

## Memory

- **Resolved: startup aborted in `StandardAllocator::Initialize`.**
  `nninitStartup` (`main+0x12ded10`) calls the helper at `main+0x12df040`,
  which initializes the allocator at `main+0x43853d0` with
  `(nullptr, 0x300000000, true)`. A probe on `sdk+0x324aa8` confirmed the
  12 GB request; the abort's backtrace ends at `sdk+0x324b7c`,
  `main+0x12ded4c`. This is the same compact-address-space limit as
  [Legends: Z-A](../0100F43008C44000/README.md#memory).
  The build-gated [cut](patches.py) at `main+0x12df074` reduces the virtual
  reservation to 4 GB. This is a platform workaround; physical commits remain
  bounded by the application pool.
- **Resolved: the 4 GB request still failed with 1 GB placement alignment.**
  The existing 62 MB node-area cuts apply because SDK build `3644a676…` is
  nirvanA Initiative's. Its allocation helper (`sdk+0x37c2d0`) rounds regions
  of at least 1 GB up to a 1 GB boundary, losing enough space to prevent the
  4 GB reservation from fitting. The additional cut at `sdk+0x37c33c` uses
  32 MB alignment, as for the equivalent SDK build in Sparks of Hope.
  Decompilation confirms region size and page guards are unchanged. The same
  startup check fails with only the main cut and reaches `nnMain` with both.
- **Resolved: 4.0.0 needs the same three cuts at its own addresses.**
  `main` `421c5411…` makes the same 12 GB request from `main+0x167d554`
  (the abort's backtrace passes `main+0x167d558`); its cut is at
  `main+0x167d54c`. SDK 13.3.13 (`bb560bdf…`) has the same
  AddressRegionManager code 0x620 bytes later: node area at `sdk+0x37c760` /
  `+0x37c7fc`, 1 GB alignment at `sdk+0x37c95c`. With only the `main` cut,
  startup still aborts in CentralHeap's setup with no SVC logged; with all
  three it reaches `nnMain`.
- **Resolved: freed startup pages stayed mapped past the application pool.**
  Startup probes all allocatable memory, frees it, and leaves those pages in
  the SDK's cache. Without `mem_map_limit`, a physical-map census reaches
  `0xd1f41000` bytes in 8 seconds, above the `0xCD500000` pool even before
  other process memory is counted. With the existing limit enabled, one
  512 KB map fails at a full pool, the SDK unmaps its `0xc8a40000`-byte cached
  block and retries. The official rewriter outputs and rebuilt host pass the
  startup check with this setting.

## Shaders

The base game's Trinity packs and `main` contain 30,142 unique shaders:
14,999 vertex, 15,057 pixel and 86 compute, with 24,487 variation pairs.
There are 40 shaders embedded in `main`. The existing Trinity reader and the
game's own Oodle decoder extract them. Chain `157e1b96c8da`, profile inputs
`6b273274b0eb`: all 30,142 compile for Mac and iPhone, with no undecoded
bindings. The complete build supplies the stamped `c1_full.tsv` and
`cb_extent.tsv`; no shader override is needed.

With 4.0.0, the tables are the union of v0 and v786432 (`shader_versions`):
39,216 unique shaders, the 30,142 above unchanged plus 9,074 the update adds,
with 35,610 variation pairs and 95 compute facts. Chain `157e1b96c8da`: all
39,216 compile for the Mac.

### Pipeline preparation depth

**Resolved:** the game calls `nvnDeviceSetDepthMode` with near = 0 and uses
`main0_z` vertex entries. The profile's `depth_mode zero` makes the offline
prebuild prepare those same bases. The first full regression exposed the
missing setting: its tool prebuild compiled 24,481 bases for near = -w with
zero failures, then the game opened those archives but rebuilt the pairs for
near = 0 before presenting a frame, missing the route's deadline. The corrected
profile then passes the unchanged 180-second route: first frame in 1.9 seconds,
frame 5,324, 29.9 fps, p50/p99 33.28/38.54 ms and no dropped frames under Metal
API validation. The shader input digest is unchanged. This is the
existing [per-depth-mode entry-point mechanism](../../../docs/reference/shaders.md#depth-mode-near--0-needs-the-per-mode-entry-points).

## Mac routes and validation

- A fresh save reaches the bedroom with `DM_KBD=Taylor` and
  `DM_KEYS="f300:A,f600:LUp+LLeft~0.3,f660:A,f1000:A,f1300:A*90/90"`.
  This selects English and the blonde girl, enters the name, confirms the
  character and advances the academy text. Let the opening movie finish.
  The get-up prompt appears around frame 10,000; `LDown~2` starts getting up.
- The bedroom save continues with `DM_KEYS="f300:A*5/150"`. The final presses
  can open the bag-and-hat dialogue; close it with A before walking. The left
  stick moves, the right stick turns the camera, and A interacts. The iOS
  profile starts with the full 3D control layout for these controls.
- [`smoke.tsv`](smoke.tsv) uses an isolated, fixed copy of the save just after
  getting up at `<NSP folder>/0100A3D008C5C000-work/smoke-save`. Make that
  copy before running the regression harness; the save stays outside the repo.
- **Initial gameplay validation, 2026-10-27, chain `7fbca6dd477d`:**
  the complete Mac shaders render the character,
  academy, movie and bedroom. A fresh 10-minute run and a 20-minute reload
  run under Metal API validation reached gameplay without a GPU fault,
  missing shader or validation error. The reload exercised walking, camera
  rotation and dialogue. A further reload walks down the stairs and around
  the lower-floor living room. Its longer run met the compiler-cache error
  below; a fresh 230-second replay reaches the kitchen and renders Mom with
  only the conservative-raster diagnostic.
  In the stationary bedroom, frame times stay around 33.3 ms (30 fps),
  resident memory settles around 3.0 GB and GPU allocations around 1.37 GB;
  this is an indoor smoke observation, not an outdoor benchmark.
- **Release route, chain `157e1b96c8da`:** all 30,142 Mac shaders compile;
  the full bedroom route passes under Metal API validation with the numbers
  [above](#pipeline-preparation-depth). The repeat using current main's RomFS
  image mount also passes: first frame in 1.6 seconds, frame 5,345, 29.9 fps,
  p50/p99 33.26/39.02 ms, peak footprint 4,329 MB and no dropped frames.
  [`usage.tsv`](usage.tsv) combines this full run with the shader features.
  Its two stationary screenshots show
  the character, room and bag-and-hat dialogue. The sole red diagnostic is
  conservative raster dilation. A preceding run had repeated display-link
  stalls and missed the frame gate; the unchanged repeat passed. The cause
  of that run's display-link stalls was not isolated.
- **Build and checks:** the unsigned IPA contains 30,142 primary iOS shader
  libraries (41,025 including variants), a 6,678,553,818-byte RomFS image and
  the expected main/SDK cuts. All ZIP CRCs pass. The Python suite passes
  449 tests (3 skipped), as do the host and full Metal graphics suites;
  pinned lint/types report no new findings.
  The final comparison of `75065df74762` against `7b00b952abc1` covers
  14 available local builds: 13 pass, with 192,870 shader records, unchanged
  MSL and no new shader or interface failures. ANIMAL WELL's full runtime
  comparison holds 59.9 fps, with screenshots and frame times within the
  harness tolerances.
  Balatro is an unsupported OpenGL `ERROR`, so the whole report is not
  labelled PASS. BotW's static checks pass; its unavailable fixed Zora save
  was explicitly skipped for runtime. Final Fantasy Tactics passed the
  earlier static comparison (`78cb26d91373` against `987759c2a218`), but its
  local build is no longer available for the final sweep. Detailed logs and
  the exact coverage record stay in
  `<NSP folder>/0100A3D008C5C000-work/release-1.0.0/`; the final report is
  `reports/full-final-2/report.md` there. All four pre-push CI jobs pass on
  the tested production source. The subsequent frontend, Mac-launcher and
  split-progress integration leaves runtime, shader-chain and iOS sources
  unchanged; 86 affected tests and pinned lint pass, and the generated iOS
  launcher settings exactly match the verified IPA.

### 4.0.0 route

- **2026-10-27, chain `157e1b96c8da`, Apple M4, macOS 27.0.1:** a fresh
  save, with the DLC's licence-only NSP in the folder, and the route above
  (`DM_KEYS="f300:A,f600:LUp+LLeft~0.3,f660:A,f1000:A,f1300:A*90/90,f11000:LDown~2"`)
  under `MTL_DEBUG_LAYER=1` reaches the academy cutscene, the Bink movie,
  the desk's get-up prompt and the standing player in the bedroom. The
  600-second run ends at frame 15,800 with conservative raster dilation as
  the sole red diagnostic and no GPU fault. Bedroom frame times: p50
  33.4 ms, p95 36.6 ms; resident memory about 5.0 GB, GPU allocations
  1,366 MB.
- Startup reaches `nnMain` only with [Bluetooth LE accessories reported absent](../../../docs/reference/services.md#bluetooth-le-accessories-reported-absent):
  4.0.0 sets up the Pokémon GO Plus + link at boot (`main+0xa0b02c`); 1.0.0
  reaches `nnMain` without it.

## Pipeline cache ownership

- **Current:** a longer indoor run reported 20 pipeline/transform-feedback
  creation or archive-write failures naming a missing
  `com.apple.gpuarchiver/gpuarchiver-db9690/PersistentState`. The log does
  not identify the process that removed it. The subsequent 230-second
  replay with folder-attribution probes reaches the kitchen without this
  error; new-library and live-base-after-flush single-process checks also
  pass. These results do not prove the original failure cannot recur.
- **Resolved: compiler-cache ownership defect.** The investigation reproduces
  deletion of a foreign `PersistentState` in both an isolated fixture and
  the real-Metal archive test. The latter fails before the shared fix and
  passes 73 checks plus separate-process compaction/restart checks afterward.
  See [Metal's build folders](../../../docs/reference/shaders.md#metals-build-folders-left-by-pipeline-archive-flushes)
  for the mechanism and fix; these tests do not identify the original run's
  deleting process.
  The initial chain's rebuilt host's 450-second replay (frame 13,200) reaches Mom's kitchen dialogue
  and commits two saves under Metal API validation, with no missing-cache
  or pipeline-creation error; conservative raster dilation remains the
  sole red diagnostic. It ends at the harness's intentional deadline.

## Files and builds

- **Current:** the base NSP passes extraction integrity checks: 24 RomFS files,
  6,369.2 MB. Modules are `main` (`ca44e7dd…`) and `sdk` (`3644a676…`).
  The host supplies the imports from Nintendo's movie module `subsdk0`.
- The complete, CRC-checked 1.0.0 IPA remains preserved at
  `<NSP folder>/PokemonScarlet-1.0.0.ipa`.
- **Resolved: an earlier 4.0.0 (v786432) copy had erased payload bytes.**
  An intact copy now builds and runs (next item); this one's
  evidence stays for reference.
  Independent checks trace 2,633 invalid 16 KB data blocks to three all-zero
  ciphertext ranges in the original NSP: 2,629 blocks in `arc/data.trpfs`
  and four in `audio/wwise/bin/Switch/DEMO.bnk`. The ranges below use absolute
  NSP offsets, with exclusive ends; they total 41,615,360 bytes (39.6875 MiB).

  | Start | End | Bytes |
  |---|---|---:|
  | `0x12c220000` | `0x12d000000` | 14,548,992 |
  | `0x12d300000` | `0x12e000000` | 13,631,488 |
  | `0x12e330000` | `0x12f000000` | 13,434,880 |

  The program NCA's signed CNMT expects SHA-256 `7e5b9bbf…`; its actual hash
  is `8ccebbb9…`. The metadata and program NCA header signatures are valid.
  The existing external checker's `SignaturePassed` result covers those
  signatures, not a recomputation of the payload hashes: signed headers and
  their hash declarations can survive while the ciphertext they describe is
  erased. Expected data-block hashes are nonzero, neighboring valid blocks
  match independently, and relocation reconstruction reproduces all 2,633
  failures. This proves source-content damage rather than an extractor-only
  failure; the event that erased the bytes remains unknown.

  The damage intersects 318 active Trinity packs, with 195 malformed pack
  metadata records aborting the full shader split. Base data reproduces the
  signed expected hash for only three blocks; 2,630 remain unrecovered. No
  original or extracted game bytes were changed. The two affected files were
  scanned completely at data-block level; the other files passed the earlier
  extractor checks. Full shader content cannot be certified from this copy.
  Detailed evidence is in the work folder's `update4/integrity/final-evidence.json`.

  User-authorized `--allow-damaged` extraction writes all five ExeFS and
  30 merged RomFS files (9,465.4 MiB of RomFS) under
  `extract-4.0.0-retry-*/unverified/`, retaining `UNVERIFIED.txt` and extraction
  results. Its code builds, `main` `421c5411…` and `sdk` `bb560bdf…`
  (SDK 13.3.13), were probed with experimental cuts and a Bluetooth LE adapter
  outside this release. That partial startup reached the language screen
  through frame 3,475 using base shaders; it was not gameplay validation.
  The investigation is preserved under the work folder's `update4/` and
  `release-1.0.0/before-scope-trim/`. No 4.0.0 build or cuts are registered in
  the profile from this copy.
- **Current: an intact 4.0.0 update.** Another dump
  of v786432 extracts without `--allow-damaged`: 35 files, 9.3 GB, with
  `main` `421c5411…` and `sdk` `bb560bdf…`. Its full shader split,
  the union tables and the [route](#400-route) come from it.
- The supplied DLC is a licence-only NSP, with no data to extract.

## Open

- [Conservative rasterization](../../../docs/reference/rendering.md#conservative-rasterization-remains-unimplemented)
  is unimplemented. The enable setter is ignored and the dilation setter
  reports `[RED] NVN unimplemented function called`. Its visual contribution
  has not been isolated; the diagnostic has not been suppressed.
- Outdoor exploration and battles have not been validated yet.
- iPhone gameplay, device memory and touch controls are untested. Audio runs
  through both Wwise output queues on the Mac without sustained underruns;
  listening on the iPhone is pending.

## Found on this game

- [StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space).
- [1 GB alignment of regions of 1 GB and more](../../../docs/reference/memory.md#1-gb-alignment-of-regions-of-1-gb-and-more).
- [Maps past the application pool fail as on Horizon](../../../docs/reference/memory.md#maps-past-the-application-pool-fail-as-on-horizon).
- [Conservative rasterization remains unimplemented](../../../docs/reference/rendering.md#conservative-rasterization-remains-unimplemented).
- [Bluetooth LE accessories reported absent](../../../docs/reference/services.md#bluetooth-le-accessories-reported-absent) (4.0.0).
- [Nintendo controllers' face buttons on the Mac](../../../docs/reference/services.md#nintendo-controllers-face-buttons-on-the-mac).
- [Pro Controller sticks missing from GameController](../../../docs/reference/services.md#pro-controller-sticks-missing-from-gamecontroller).
- [Metal's build folders left by pipeline archive flushes](../../../docs/reference/shaders.md#metals-build-folders-left-by-pipeline-archive-flushes).
