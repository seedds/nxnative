# Reuse shader stages and fix split-block BRX analysis

## Current PR after rebasing on main

Rebased onto upstream `7dc00e4d` (including the latest lint and packing fixes). Main now implements several optimizations explored in the original project. This revision keeps upstream's PCH implementation, compilation through standard input, shared pixel compile units and subsequent splitting into base/side libraries. In particular, it preserves upstream's fix for oversized pipeline archives when all entries remain in one library. The older `dmsh_onelib.py`, PCH adapter and fixed-filename shim have been removed from the current patch.

The remaining changes add:

- Compressed, verified intermediate translation/MSL caches, including geometry readers and integer-input retranslations.
- Optional pinned SPIRV-Cross C API conversion, with isolated contexts, tool identity checks and CLI fallback.
- Indexed reflection/fetch analysis and literal-prefiltered entry scanning, adapted to upstream's `dmsh_side.py`.
- Shared in-flight Metal requests for both whole-library compilation and upstream's two-piece `compile_split` path, with shared failure propagation. The concurrency regression checks both pieces as well as ordinary libraries.
- The generic decoder predecessor-link fix for BRX analysis, rebuilt against the current upstream translator sources. Upstream's newer task/mesh support and other decoder changes are preserved.
- Numerical GPU fixtures adapted to upstream's shared pixel compile unit, plus cache, parser and translator regression tests and documentation.

The current chain is `b2da9a0d5416`. Historical build measurements below belong to chain `9d2977751074`; they are not measurements of this rebased implementation. The installed player app was not replaced during this rebase.

## Rebase verification

- The managed translator published successfully from the combined sources; the universal apphost is preserved.
- The Pokopia Mac host built successfully on the final chain (`make -C dm/host GAME=0400A3A01EE4C000 -j8`); the managed translator also published successfully.
- Focused compiler/cache checks: 101 tests passed with one GPU opt-in test skipped. After the final code change, 80 focused cache, decoder, chain and documentation tests passed with no skips.
- Graphics Python suite: 321 tests passed, five skipped, with `DM_GAME` and the rebuilt translator set explicitly.
- Final-base tool suite: 1,087 tests passed, nine skipped.
- Complete Metal graphics suite: `make -C dm/gfx/tests GAME=0400A3A01EE4C000 run` passed, including the adapted numerical fixtures and current upstream alpha/logic paths. Compiler warnings in unchanged upstream host code remain visible in the build log.
- Python lint ratchet against `origin/main`: 387 baseline findings, zero new findings, zero stale baseline entries.
- Five-game regression was attempted for Pokopia, Wonder, Breath of the Wild, Tears of the Kingdom and Odyssey. The harness reports no prepared Mac builds for any of them under the available game-folder root. That gate remains incomplete; installed apps do not replace extracted build inputs.
- A full game shader rebuild and gameplay comparison on this new chain have not been performed. Prior complete-build and user-playtest evidence is retained below with its original scope.

<details>
<summary>Original project investigation, historical benchmarks and validation</summary>

The following writeup describes the original implementation before upstream incorporated alternative implementations of the overlapping optimizations. See the current scope above for what this rebased PR actually changes.

# Reduce full shader build time and fix BRX translation after block splits

## Summary

Large NVN shader builds remained slow after the package-memory fix in `e70fd69d5c828c906905991872c2eeead435dd4e`. This change reduces repeated Metal compilation, source scanning and converter launches, improves cache reuse, and fixes a control-flow graph defect that prevented 16 otherwise valid shaders from translating.

The completed Pokémon Pokopia v327680 Mac build contains **131,382 successful shader keys, with zero shader failures**. The production pass took **3 h 55 min overall**, including **3 h 47 min for the shader stage** and **5 min 21 s for pipeline prebuild**, on a 10-core M5 MacBook Pro with 16 GB RAM and `J=12`. The resulting Mac app includes every base shader library and a pipeline archive for its GPU.

All shaders remain in the build. Alpha testing, logic operations, integer outputs, dual-source outputs and the existing vertex/geometry paths retain their supported variants and fallbacks. These changes apply to the shared toolchain rather than selecting a smaller Pokopia shader list.

## Problem and diagnosis

The original report was a full shader build taking more than ten hours despite the upstream fix. That fix addressed a real problem: retaining approximately 4.4 GB of packages inflated the builder and made subprocess creation hold the GIL long enough to serialize workers. This PR builds on that fix; it does not replace or claim credit for it.

Profiling the remaining work identified several independent costs:

1. Metal repeatedly instantiated its standard-library texture templates for each shader compilation.
2. A pixel shader's base, alpha-test and logic-operation libraries repeatedly parsed largely identical source.
3. Python reflection and variant analysis scanned large generated sources once per member or generated identifier.
4. Identical concurrent Metal requests could both observe a cold cache and compile the same source.
5. Downstream edits invalidated useful translation and SPIRV-Cross intermediates.
6. Every uncached MSL conversion launched another SPIRV-Cross process.
7. A separate decoder bug left stale predecessor links after block splitting, causing 16 real BRX failures.

A representative 312 KB pixel shader spent approximately 610 ms of a 920 ms Metal compile instantiating texture types. Eliminating only package-memory overhead could not remove that repeated compiler work.

## Implementation

### Precompile Metal texture types

`dmsh_pch.py` builds one header per target with the target's own flags. It instantiates the common texture types, including both memory-coherence forms used by argument buffers, and starts in the background while cold shaders translate. Cached builds start it lazily when a Metal compilation actually needs it.

A probe compares AIR produced with and without the header. A failed build or unequal probe disables the header; a shader rejected with the header is retried without it. `DMSH_PCH=0` remains available for comparison. The header changes compilation cost, and is deliberately excluded from the semantic chain fingerprint.

### Compile pixel variants together

`dmsh_onelib.py` factors the translated body into a helper and supplies separate base, alpha-test and logic-operation entry points in `<key>_b.metallib`. The compiler chooses whether to inline the helper. Forcing inlining duplicated large optimized bodies and increased compilation cost.

Reflection records the library and `_at` / `_lo` entry suffixes. `nvn_mtl.m` uses that metadata when loading functions and registering them for pipeline archives, while retaining the existing separate-library path. Existing argument-buffer compatibility checks remain in place.

`_b.metal` records the actual compiled merged source. `_b.plain.metal` retains the original source needed by later analysis and cache restoration. Both are restored with the corresponding metadata. Unsupported source shapes retain the original path; a merged compilation failure rebuilds the original base and side libraries.

Comparison controls remain available:

| Setting | Behavior |
|---|---|
| `DMSH_ONELIB=1` | Base and side entries in one library; default |
| `DMSH_ONELIB=side` | Combined side library, separate base |
| `DMSH_ONELIB=0` | Separate base, alpha-test and logic-operation libraries |

### Reduce repeated source scanning

Reflection indexes member operations once. Fetch analysis indexes declarations and assignment counts instead of compiling and running one regex per generated name. Bracket matching jumps between delimiters. Atomic emulation avoids an unnecessary regex when the required literals are absent.

`dmsh_scan.literal_lines` locates candidate lines using a required literal, then lets the original anchored regex determine the match. Tests retain qualifiers, multiline signatures, capture groups and match positions. This avoids replacing the shader parser with a looser approximation.

### Share identical Metal compilations and stabilize their identity

`dmsh_mtlcache.Cache` holds a future for each in-flight source/compiler/target key. Concurrent callers share one result or failure; unrelated keys still compile independently. Failed requests do not publish a library.

Metal can encode the input basename in internal symbols. Recompiling saved `<key>_b.metal` therefore did not always reproduce the library originally compiled as `<key>.metal`, even without debug information. Workers now compile under the fixed basename `shader.metal` in private directories. The convention participates in the compiler record and is used for cache import checks too.

### Cache translation and source conversion separately

`dmsh_stages.py` stores compressed, checksummed translation and initial MSL results. Stage identities include the relevant compiler/tool inputs and semantic arguments. Translation retains resource metadata and geometry-reader outputs; integer-input retranslations remain distinct. Initial MSL keys include the unmodified input SPIR-V and conversion options.

Only successful stages are atomically published. Missing, incomplete or corrupt entries are rebuilt. Temporary paths do not invalidate equivalent inputs. Later rewrites and Metal compilation keep their own cache identities. `DMSH_STAGE_CACHE=0` disables intermediate reuse for comparison.

Pixel factoring, alpha-test and logic helpers have a separate `PIXEL_FP`, avoiding invalidation of unrelated stages. Paired vertex shaders still include their pixel dependency digests.

### Use the pinned SPIRV-Cross C API

`dmsh_cross.py` optionally uses a universal shared library built from the same pinned SPIRV-Cross revision as the CLI. Every call owns a separate compiler context; ctypes releases the GIL during compilation. The adapter, backend identity and binary fingerprint participate in cache/toolchain identity.

A missing shared library, unmapped option or native conversion error uses the CLI, preserving its coverage and diagnostics. `DMSH_CROSS=cli` forces that path. The build script produces both the CLI and shared library; no SPIRV-Cross revision upgrade is included.

### Repair predecessor links in the translator

`Decoders/Block.cs::Split` moved instructions and outgoing edges into the right block, but successor blocks still listed the old left block as a predecessor. Backward BRX analysis could therefore miss an LDC instruction that had moved into the right block.

For shader `8162866c3dcb72cb`, BRX at `0x7770` reads R20. LDC at `0x70d0` loads its valid c1 table at `0x790`. Splitting block `0x6f60` at `0x7060` moved that load, while the stale incoming edge sent analysis toward an earlier writer at `0x7010`.

The fix replaces outgoing successors' predecessor references with the right block **before** adding the new left-to-right fallthrough edge. This order also handles self-loops. It repairs the graph rather than guessing jump targets, suppressing the exception or skipping shaders.

The pinned managed translator outputs and lock were rebuilt, and `DMSH_CHAIN` is `9d2977751074`. The original universal apphost was preserved.

## Measurements

### Completed production build

| Item | Result |
|---|---|
| Game/build | Pokémon Pokopia, v327680 derived profile |
| Hardware | 10-core M5 MacBook Pro, 16 GB RAM |
| Workers | `J=12`, selected through a worker-count study |
| Chain | `9d2977751074` |
| Full shader result | 131,382 successful keys, zero failures |
| Shader stage | 3 h 47 min 17 s, including final metadata generation |
| Pipeline prebuild | 17,939 pairs in 320.8 s; 17,935 bases built, zero failures |
| Overall development build | 02:19–06:14 local on November 4, 2026; 3 h 55 min |

Four layer-writing pairs use the existing runtime pipeline path. Their shader libraries are included.

**Timing qualification:** this was a complete-coverage production pass, not a rigorously empty-cache benchmark. Sixteen completed shaders were restored from validation, and earlier work had populated lower-level caches. The initial ten-plus-hour report and earlier rate estimates are useful context, but do not establish a controlled end-to-end speedup percentage. Sample improvements below are separate experiments and must not be multiplied together.

### Paired samples

| Experiment | Before | After | Scope |
|---|---:|---:|---|
| PCH and initial scanner changes | 139 s | 79 s | 250 cold shaders; 1,217 outputs matched, excluding the reflection chain stamp |
| Combine alpha/logic side libraries | 98.03 s | 76.47 s | 120 shaders, PCH enabled in both runs |
| Merge base and side entries | 44.90 s | 32.05 s | 120 shaders; 111 merged pixel libraries, no merge failure |
| Let Metal choose helper inlining | 141.09 / 140.56 s CPU | 90.66 / 91.41 s CPU | 111 real pixel shaders |
| Index reflection member operations | 0.824 s CPU | 0.037 s CPU | Median over 60 real shaders |
| Prefilter entry scans | 1.507 s CPU | 0.046 s CPU | 300 lookups over 100 merged sources |
| Index fetch declarations | 1.832 / 1.834 s CPU | 1.259 / 1.234 s CPU | 128 real pixel shaders |
| Further entry/factoring scans | 1.660 / 1.640 s CPU | 0.763 / 0.754 s CPU | Same 128-shader study |
| Native SPIRV-Cross | 2.769 / 2.913 s | 2.149 / 2.143 s | 129 modules; identical MSL |

The inlining study reduced AIR from 24,076,992 to 7,588,816 bytes. A synthetic heavy GPU shader showed no slowdown; this is not a claim of higher gameplay FPS.

A mixed-stage sample built 129 shaders and compared 491 outputs. With Metal already cached, preparation took 9.09 s with the previous scanners/CLI, 8.17 s with the new path and cold intermediate caches, and 6.33 s with reused intermediates. A completed-shader cache hit took 3.24 s. Source and library bytes matched; seven vertex JSON files differed only in key order. The 258 intermediate entries occupied 18,517,821 bytes versus 73,583,523 uncompressed.

Exact-source deduplication was retained. A census found 2,157 distinct sources among 2,210 inputs; whitespace/comment removal or generated-name normalization found no further benefit, so no such normalization was added.

## Correctness and validation

Completed during implementation:

- All 16 formerly failing real BRX shaders translated and compiled into Mac Metal libraries.
- Comparing the decoder fix against its predecessor produced byte-identical SPIR-V and resource payloads for all 131,366 previously successful shaders: zero missing and zero changed. This comparison isolates the graph fix; it does not claim merged Metal binaries have identical instructions.
- Eight synthetic translator control-flow tests passed, including a reproducer that fails before the graph fix and a self-loop regression case.
- GPU fixtures matched 73,728 numerical components bit for bit with production `-O1 -fpreserve-invariance`, including merged libraries with and without PCH.
- Expanded host replay passed 2,364 alpha-test and 2,761 logic-operation checks.
- The heavy inlining fixture matched 524,288 components in each of six alternating trials.
- PCH AIR equality, native/CLI conversion parity, concurrent contexts, image atomics, cache corruption/publication, cache invalidation, shared failures, canonical filenames and merge fallback were exercised.
- The Metal graphics suite passed during development. The later decoder change was additionally covered by the real-shader compilation and full SPIR-V/resource comparison above.
- Earlier complete Python runs reported 254 graphics tests with five skips, and 1,008 tool tests with 26 skips. The Python lint ratchet reported 389 baseline findings and zero new findings at that point.

Fresh writeup-time verification:

- Focused cache, PCH, native conversion, merge, scanner, control-flow, chain and documentation suites: **93 tests, three skips, success**. Compiler/GPU opt-in cases were not enabled in this focused rerun. ResourceWarnings were emitted by file handling; the run was not warning-free.
- `tool_lock.check_tools()` returned no mismatches; computed chain remained `9d2977751074`.
- `git diff --check` passed.

### Mac app and gameplay

The self-contained Mac app was packed and its ad-hoc signature passed `codesign --verify --deep --strict`. Bundle inspection found all 131,382 base libraries and their reflection files, with zero missing outputs.

The first packaged launch revealed that this v327680 build calls `nvnDeviceSetDepthMode` with near = 0. An archive made for the default GL depth mode was therefore rejected. The derived profile was corrected to `depth_mode zero`, the archive was rebuilt, and the app was repacked. This was a local derived-profile/packaging correction, not an additional shader translation change or a committed change to the registered v655360 profile.

The corrected archive is approximately 445 MB. The packaged host found all 16 sampled bases and skipped a full pipeline prebuild. Automated inspection reached English menus, the opening, character creation and the first rendered 3D scene, with working scripted controls. The user subsequently confirmed the game was working. This is not full-campaign coverage or a deterministic cross-revision gameplay comparison.

The muted automated launch was followed by a normal sound-enabled launch: real stereo output at 48 kHz, 240-frame callbacks and zero underruns in the observed intervals. The Metal performance HUD was restored through player settings; those settings and saves are not repository changes.

The later startup count of 8,097 shaders is the user's recorded warmup set, not the number packaged. The installed app still contains all 131,382 libraries. That warmup took approximately 1.9 seconds in the observed launch.

## Remaining validation and limitations

- The five-game regression gate is **not complete**. The earlier harness attempt did not find the required prepared local builds. Do not present the successful Pokopia build as a five-game regression pass.
- A suitable follow-up matrix is Pokopia (large NVN2 build and BRX cases), Super Mario Bros. Wonder (NVN pixel variants), Breath of the Wild and Tears of the Kingdom (pipeline/vertex paths), and Super Mario Odyssey (large shaders). This is a proposed selection, not five tested games; confirm it with `regress.py impact` for the eventual commit range and attach the actual harness report.
- A full iOS shader build, IPA and device gameplay run were not performed. Targeted iOS compiler fixtures are not equivalent to those checks.
- The dump is v327680, while the registered Pokopia profile targets v655360. Its derived build does not inherit unverified fixed-address patches for another NSO build.
- The full-campaign game state, long-session performance, broad engine coverage and device memory behavior remain outside the demonstrated checks.
- The packaging rebuild restored cached outputs and reran metadata analysis. That cache-hit pass took about 42 minutes for the shader stage despite needing no shader recompilation. Caching verified final tables and reusing intact complete outputs are future opportunities, not improvements implemented here.
- Increasing pipeline worker count and reusing loaded libraries/reflection across prebuild pairs are also unbenchmarked follow-up ideas. `J=12` is a measured local setting, not a new universal default.
- Historical benchmark scratch and the original external build/work folders are no longer available at their earlier paths. The measurements above come from the recorded session and repository notes; do not promise those raw logs as attached PR artifacts.

## Reproduction and review

Build a complete Mac development artifact from a legally obtained game folder:

```sh
J=12 python3 scripts/build_ipa_from_nsp.py "/path/to/game-folder" --platform mac --progress lines
```

For the self-contained app, use `--platform mac-app`. Give each game its own shader output directory, and verify the final `_b_build.json`, library/input stamps, generated tables and variant failure counters; a successful outer process exit alone is insufficient proof of full shader coverage.

Focused verification used for this writeup:

```sh
DMSH_TEST_XLATE="$PWD/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate" \
python3 -m unittest \
  dm.tools.tests.test_dmsh_stages dm.tools.tests.test_dmsh_cross \
  dm.tools.tests.test_dmsh_pch dm.tools.tests.test_shader_toolchain_cache \
  dm.gfx.tests.test_dmsh_scan dm.gfx.tests.test_dmsh_onelib \
  dm.gfx.tests.test_dmsh_control_flow dm.tools.tests.test_nsp_chain \
  dm.tools.tests.test_docs
```

Review the shader builder/cache changes, merged-library host loading, and decoder graph fix as distinct concerns. Rebuilt managed translator outputs, the universal SPIRV-Cross shared library, lock/header, new helpers, tests, documentation and impact-map entries are intentional parts of the change. Several new files are currently untracked and must be included when preparing commits.

The pre-existing edit to `dm/games/010015100B514000/README.md` and untracked `full_build.txt` are unrelated to this project and should be excluded. The Mario build transcript is not validation evidence for this patch. Game data, keys, caches, saves, packaged apps, launchd wrappers and benchmark scratch do not belong in the PR.

This document is a PR-body draft; it can be pasted into GitHub without committing the draft itself. No commit, push or pull request was created as part of preparing it.

</details>
