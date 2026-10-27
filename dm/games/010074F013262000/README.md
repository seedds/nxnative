# Xenoblade Chronicles 3

Base title ID `010074F013262000`, update 2.2.1 (`v720896`), SDK 12.3.9,
Monolith's engine, NVN. Modules: `main`, `subsdk0` (NintendoSDK_movie), `sdk`.
The build IDs are in [game.tsv](game.tsv).

## Current status

Opening gameplay tested on an Apple-silicon Mac (2026-10-27). The compact layout
renders the title, terrain, characters, lighting and effects. New-game setup,
opening movies, the first enemy defeat, destination tutorials, walking and the
engagement-area collision boundary work. All 27,055 base/update shaders compile
for Mac and iOS. The unsigned IPA is built and its archive checks pass.

iOS runtime testing, DLC gameplay and later-game allocation peaks remain open.
The supplied four DLC packs are included; their additional shaders have not been
integrated into the base/update shader census, so DLC rendering is unvalidated.

## Profile and memory

- The NPDM declares a 32 MB system resource. Memory is committed through address
  regions, without `SetHeapSize`. The profile uses a 5 GB guest block, including
  a 4.375 GB alias region, and the Switch application pool of `0xCD500000` bytes.
- `mem_map_limit on`: startup attempts 1 MB physical commits until allocation
  fails, then frees its probe. An unlimited host lets it map all 60 GB; enforcing
  the hardware pool makes that probe stop at the expected physical limit.
- **Address-space adaptation:** `nninitStartup` calls `main+0x128e58c`, reserving 60 GB.
  Its first arena reserves 4 GB + 128 KB (`main+0x1241860`), so reducing only the
  containing region cannot work. [patches.py](patches.py) changes that arena's
  bump increment, subtraction and capacity together, to 512 MB + 128 KB, and its
  containing region to 4 GB. All changes are gated by the 2.2.1 main build ID.
  The SDK build takes the existing node-area and large-region alignment cuts.
- A 40-second diagnostic run with the original arenas had 2,092.4 MB committed:
  about 203 MB in the first arena, about 1.8 GB in subsequent windows, and a 64 MB
  allocation near the end of the reservation. Original-layout opening battle
  (`boot29wide`) commits 225.44 MB in the first arena. That leaves about 286 MB
  of capacity in the compact first arena; later-game peaks remain unvalidated.
- Compact runs stayed at the title for over ten minutes without an allocator
  failure. This validates the title allocation pattern, not later gameplay peaks.
- `pad_layout full` supplies both sticks and the D-pad.

## Files and shaders

- The merged RomFS is 14.15 GB: 238 WebM movies, nine Wwise PCKs, and `bf3.arh` /
  `bf3.ard`. The ARH1 index has 76,445 files; the ARD is 9.05 GB. Archive entries
  and model/map streams use XBC1 (zlib and zstd).
- The full base/update split finds 27,055 shader keys: 8,201 vertex, 18,533 pixel,
  308 geometry and 13 compute, with 17,648 variation pairs. `main` supplies 204;
  archive assets supply 26,851. All compile for Mac and iOS with feedback entries
  (chain `7fbca6dd477d`, profile inputs `2342f43192c3`, after integration with main).
- **Resolved, extraction:** six boot shader stages live in one standalone SLCT
  at `main+0x14e01c0`, without an HCPS header. Control and code offsets are relative
  to the SLCT itself. Its control sections include assembly text and are aligned
  to eight bytes. The first pair's keys, `58e2e00d24c6f3ea` and
  `5d36d4144f49ec9a`, match runtime dumps exactly.
- Twelve more keys captured during startup match the extracted archive programs.
  Equal code can have different compiler identity bytes at control +0x7d0; all
  such keys are retained, with their original control bytes.
- **Resolved, extraction:** `/effect/cmn_assets.wipac` contains complete GLSLC
  outputs alongside HCPS programs. Reading them adds 914 base-game keys. Captured
  `6babc936d58fcd8f` (vertex), `7a7eb30dfb6187b5` (geometry) and
  `b540e3f8e28bd41e` (pixel) match these outputs exactly.
- **Resolved, translation:** vertex `4b98320a0f512efe` reads `SubgroupLtMask`
  through a component pointer; the general one-lane lowering now covers that form.
- **Resolved, title rendering:** the black foreground model had valid G-buffer
  colour and normals, but the separate IMAGE stencil descriptor read empty
  storage. The general sharing fix restores lighting (`boot17shared`, frame 1200).
- **Resolved, opening deadlock:** the output thread held the host decoder mutex
  while signalling the higher-priority input thread; the handoff waited for that
  input thread, which was blocked on the same native mutex. Annotating contended
  native waits restores playback (`boot18movie`, `mv01005100.webm`, 140 seconds).
- **Current, native audio setup:** a later real-audio run (`boot25full`) stopped
  presenting while `AKEventManager` joined the Apple audio-unit setup helper.
  A native join must release the guest core, just like a contended codec lock;
  `dm_host_thread_join` now preserves that scheduler accounting. Gated join,
  nesting and audio-cycle regressions pass. The sample does not establish that
  Apple audio-unit construction itself would remain blocked indefinitely.
  The corrected compact run continues for over ten minutes with real output
  `#0`, without the diagnostic watchdog or reported audio underruns. It does
  not reopen the output, so the earlier construction stall is not isolated.
- **Resolved, nested effect compression:** those four geometry/pixel keys, and
  vertex `d021f6a3763a9327` / pixel `1c6c6a015f6f3acf`, match GLSLC programs inside
  `/effect/fld/ma01a.wipac` after its additional `cmp\0zlib` layer is decoded.
  The expanded base/update census is 27,055 keys (8,201 vertex, 18,533 pixel,
  308 geometry, 13 compute), with 17,648 variation pairs.
- **Resolved, vertex work under discard:** `cf66cef403c57845` and
  `36710e69a07e849d` write bone palettes through storage buffers. Running their
  vertex work restores geometry (`boot21discard`), initially with large distorted
  surfaces. `boot22full` reproduced this with the complete shader set. A native
  post-vertex capture of the first `4f6b050575d4de0b` draw gives 2,080 finite
  vertices inside the clip volume. The remaining corruption was the primitive
  ordering failure below.
- **Resolved, feedback:** effect geometry `c01e3ade8e15a842` captures eleven
  interleaved generic outputs (172 bytes per vertex). The general feedback path
  now decodes these byte component counts, captures geometry output and saves
  GPU draw counts. Native regression checks pass; `boot23feedback` runs this path
  without missing feedback calls or capture-entry errors.
- **Resolved, exploding meshes:** the game orders bone-palette work with
  `ORDER_PRIMITIVES` barriers (`0x21`). The host only split render encoders for
  fragment barriers after storage writes. Enforcing primitive ordering removes
  the exploding model parts (`boot24barrier`).
- **Resolved, terrain allocation:** `boot27draw` frame 4000 replays every
  recorded program bind/draw, with no map shader or rejected draw. CPU probes
  (`boot28cpu`) find the map owner rejected before visibility because its core
  buffer at `+0xd18` is null. The 390 MB allocation (`boot30alloc`) needs a
  512 MB virtual window. `boot32region` at `main+0x12429a4` records bump
  `0xe46c0000` → `0x1046c0000`, beyond the compact 4 GB container, before any
  physical mapping. The heap's byte limit is disabled (`heap31.bin`). Reducing
  the first arena's reservation from 1 GB to 512 MB frees that window; this
  retains the Switch physical-memory pool and all map work. The original-layout
  battle's reservation sequence predicts 159.56 MB of remaining outer space;
  corrected compact gameplay (`boot33compact512`) confirms bump `0xf6070000`,
  a 512 MB + 128 KB first-arena capacity and about 225 MB committed there.

## Mac validation and remaining work

`boot33compact512` uses the complete shader set, the corrected compact modules
and real audio for over ten minutes. It defeats the first enemy, advances through
the destination and tips tutorials, then walks across Everblight Plain to the
engagement boundary.
`run/compact33_terrain.png`, `compact33_move.png` and `compact33_walk.png` retain
these milestones. Alias mappings settle near 2,653–2,655 MB during this sequence.
Later areas, saves/reloads and DLC have not been exercised.

The unsigned iOS app compiles with signing disabled. The IPA is 20,629,976,490
bytes (19.21 GiB), contains all 27,055 base/update shader libraries and four
mounted DLC directories, and passes every ZIP CRC. It has no code signature or
provisioning profile. No device installation or runtime test was performed.
The delivered IPA is the original validated `bab889ccd129` build; integration
with main rebuilt both platforms' libraries but did not replace that artifact.

After integrating main at `6f58a7d`, `boot34main` repeats the first enemy defeat
and destination tutorial with the combined host and shader chain. The full Metal
suite, all 48 Python test files, all 178 iOS source compile checks and the pinned
lint/type ratchet pass. A fresh launch after shader metadata generation accepts
the new library/table stamps. The all-local-game regression harness was started
but stopped during its first baseline compilation, before any comparison
completed; no cross-game comparison coverage is claimed for that sweep.

## Mac development files

The supplied folder is `/Volumes/S790/nsp/Xenoblade Chronicles 3/`. Extraction,
the shader packages and isolated hosts are in `010074F013262000_build/`; analysis,
private keys, Python tools and isolated runs are in `010074F013262000-work/`.
`run/boot.py` accepts a log name, duration and environment assignments. Diagnostic
logs `boot01` / `boot02` show the original allocator failure; `boot03wide` through
`boot07wide` retain the wider-reservation experiments. `boot07wide` ends cleanly
after 40 seconds; it uses an empty shader map and is not a visual test.

Use `patched_512/` with `tools/dmhost_512` and `shader/fx_cache/` for the validated
Mac layout; the earlier `patched_compact/` has the insufficient 1 GB first-arena
reservation. `DM_NVN_SHMAP` points to this profile's `shader_map.tsv`.
On the title/new-game screens press A. The auto-attack tip advances with R and
closes with A; stand near an enemy to auto-attack and use the left stick to walk.
During a movie, Plus then a held X skips it. The diagnostic watchdog is off when
`DM_WATCH` is absent; setting it to `0` still enables its default stall threshold.

## Found on this game

- [Monolith ARH1, XBC1 and HCPS/SLCT shader containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers).
- [Subgroup operations rewritten for a one-lane subgroup](../../../docs/reference/shaders.md#subgroup-operations-rewritten-for-a-one-lane-subgroup).
- [IMAGE usage does not split texture storage](../../../docs/reference/rendering.md#image-usage-does-not-split-texture-storage).
- [Rasterizer discard preserves vertex storage writes](../../../docs/reference/rendering.md#rasterizer-discard-preserves-vertex-storage-writes).
- [Primitive barriers order vertex storage writes](../../../docs/reference/rendering.md#primitive-barriers-order-vertex-storage-writes).
- [Transform feedback captured by a vertex entry point](../../../docs/reference/rendering.md#transform-feedback-captured-by-a-vertex-entry-point): interleaved outputs and geometry capture.
- [A host lock held across a same-core handoff](../../../docs/reference/threads.md#a-host-lock-held-across-a-same-core-handoff): contended native decoder waits must release a handoff.
- [oe calls answered without an applet proxy](../../../docs/reference/services.md#oe-calls-answered-without-an-applet-proxy): enabling album-image notifications dereferenced the absent SelfController.
- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area) and [1 GB alignment of regions of 1 GB and more](../../../docs/reference/memory.md#1-gb-alignment-of-regions-of-1-gb-and-more).
