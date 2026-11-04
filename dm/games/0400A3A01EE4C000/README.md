# Pokémon Pokopia (`0400A3A01EE4C000`)

A Switch 2 game: update v655360 (the title screen says Ver. 2.0.1), SDK 20.5.25 built for the Switch 2 (`Ounce`). Koei Tecmo's KTGL engine on NVN2, modules `main`, `subsdk0` (WebM movies) and `sdk`. **Plays on the Mac**: the opening, the character choice and the first area's tutorial at about 60 fps, including 31 minutes of the user's playtest with no GPU error and no crash. A save from an earlier run loads and continues. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x270000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Donkey Kong Bananza's 9 GB layout, not measured here. The NPDM declares no system resource |
| `mem_used 0x5600000` | `game.tsv` | the rewritten modules take 82 MB, more than the default 64 MB that `SetHeapSize` checks the game's request against |
| `pad_layout full` | `game.tsv` | |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 (the log's `depth mode` line). Without the key the pipeline archive made offline holds the other entry point, and the host refuses it |

## Files

- A dump folder whose `updates/<version>/` holds the whole RomFS; [`pack_dump.py`](../../../scripts/pack_dump.py) packs it without a base RomFS. RomFS: the newer RDB (as ROMANCE OF THE THREE KINGDOMS 8 REMAKE's) under `motor_asset_re/KIDS_PLATFORM_OUNCE`, and a `LD_C0_R.BIN` / `LI_C0_R.IDX` LINKDATA pair.
- Shaders: 131,426 in [`shader_map.tsv`](shader_map.tsv) ([KTGL on NVN2](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata)). The Mac playtest's library held only the 8,157 that its runs asked for (with their pixel shaders), listed in [`shader_seen.tsv`](shader_seen.tsv); the NSP script's `--seen-shaders` builds those alone ([Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders)). Such a library has no stamp and no `c1_full.tsv`, and each start logs that in red. On chain `20b9b1c2e4e4` an M4 Pro with other jobs running took 18 minutes for the 8,158 shaders of the list and the last logs (Metal's compile cache held nearly all of its libraries: 113 compiled), or 16 minutes with all of them cached, then about 5 minutes for the 7.9 GB Mac app. That app boots to walking at about 16.7 ms a frame with no GPU error and no shader reported missing, and later areas are untested on it. The NSP script builds all 131,426: an estimated 6.4 hours on an M4 Pro (from 1,019 shaders; no full build has run) since [the builder stopped keeping the 4.4 GB of packages in memory](../../../docs/reference/shaders.md#a-shader-build-that-keeps-its-packages-in-memory-runs-one-worker-at-a-time) and [its compiles stopped instantiating Metal's texture classes again](../../../docs/reference/shaders.md#each-metal-compile-instantiates-metals-texture-classes-again) (before both, players saw about one shader a second and ETAs of 20 to 200 hours: GitHub issue #1036). The map's 105,534 pixel shaders each have `_ba` and `_bl` libraries, 57% of its compiles before those whose MSL matches another's, while the runs on the 8,157-shader library opened none of them (file access times; 7,985 of its 8,157 `_b` libraries were opened). Those runs only reached the first area (open).
- No `pso_manifest.tsv` yet, so each start also logs a red line for the missing seed list. In the runs here no draw waited on it.

## Shaders

- Resolved: `cc679c7b4d3e703c` did not translate (`LDC with a bindless bank or an address mode`): an indexed read of c1 with the bank in the address register, found on Hyrule Warriors: Age of Imprisonment, which ships the same shader ([LDC with the bank in its address register](../../../docs/reference/shaders.md#ldc-with-the-bank-in-its-address-register-is)). All 8,158 shaders of the upstream v655360 playtest library also translated. The v327680 full pass on 2026-11-04 translated 131,366 of 131,382 unique keys; the earlier LDC and f55 failures did not recur. The remaining 16 BRX failures are resolved below.
- Resolved: all 16 `BRX with no target` failures came from [stale predecessor links after splitting a block](../../../docs/reference/shaders.md#split-shader-blocks-retain-stale-predecessor-links). In `8162866c3dcb72cb`, the BRX at `0x7770` reads R20, loaded from the valid c1 table at `0x790` by the LDC at `0x70d0`. Splitting block `0x6f60` at `0x7060` moved the load but left successors pointing at the left block; backward lookup rejected an earlier writer instead. The generic graph fix makes all 16 translate and compile into merged Mac libraries. Eight synthetic control-flow tests pass, including a new reproducer that fails before the fix and a self-loop case; A full comparison then confirmed byte-identical SPIR-V and resource JSON for all 131,366 previously successful shaders; only the 16 formerly failing translations were added. The corrected full Mac pass compiled all 131,382 unique shaders successfully; packaged-app checks reached the opening and first 3D scene, followed by user-confirmed working gameplay.
- A `3D binding fallback` line fires once at about frame 2100: a 2D 8x8 texture in a 3D slot. The shader is not identified (open).

### Build performance

The measurements below describe the original branch at chain `9d2977751074`. After rebasing onto upstream `7dc00e4d`, the branch retains upstream's PCH, stdin compilation and split base/side libraries, and adds intermediate caching, native conversion, indexed analysis and the BRX fix. The full v327680 game build and gameplay have not been repeated on the rebased chain.

Current implementation: precompiled Metal texture types, one shared body for each pixel shader's base/alpha-test/logic-op entries, and concurrent identical-source cache requests sharing a compile. No shader subset or feature cut is selected. A full Mac build of the user's v327680 dump started on 2026-11-03 with chain `74768d493e22`, `J=12`, and 131,382 unique shader keys. That pass was stopped at the user's request, with 1,144 base libraries cached. Full validation on 2026-11-04 exposed the BRX graph defect above. The corrected full pass ran from 02:19 to 06:14 local with chain `9d2977751074` and `J=12`: 3 h 55 min overall. All 131,382 shader keys succeeded (16 restored from cache); the shader stage took 3 h 47 min including roughly 15 minutes of final metadata analysis. Pipeline prebuild then processed 17,939 pairs in 320.8 s, building 17,935 bases with zero failures; four pairs use the normal runtime pipeline path. Packaged-app checks reached the opening, character creation and the first 3D scene; the user confirmed working gameplay. This is not a full-campaign or deterministic cross-revision regression result.

Paired sample measurements on the 10-core, 16 GB M5 MacBook Pro (compare within a row; separate runs had different desktop load):

| Change | Sample | Before | After |
|---|---|---|---|
| Precompiled texture types and Python scanning fixes | 250 shaders, cold cache | 139 s | 79 s |
| Combined alpha-test/logic-op side library | 120 shaders, `J=10`, precompiled types in both | 98.03 s | 76.47 s |
| Base and side entry points in one library | 120 shaders, `J=10`, precompiled types in both | 44.90 s | 32.05 s |

The last sample built all 120 shaders: 111 merged base libraries, no failed merge; a cache-hit repeat took 7.05 s. GPU numerical fixtures matched 73,728 components with the production `-O1 -fpreserve-invariance` flags, including merged libraries with and without the precompiled header bit for bit. Expanded host replay passed 2,364 alpha-test checks and 2,761 logic-operation checks. These fixtures cover arithmetic, non-finite inputs, branches, derivatives, texture reads, discard, integer outputs and argument buffers; they do not replace gameplay validation.

The source census of the 2,210 available shader sources found 2,157 distinct sources (53 repeats). Removing comments/whitespace or renumbering generated identifiers found no additional matches; no such normalization was added. The cache concurrency test makes eight identical cold requests and verifies one compile, seven shared results and shared failure propagation. Its full-build savings are not measured yet.

The worker-count study used 40 merged sources with the texture header ready. `J=12` took 7.059 / 7.080 s, `J=10` 8.626 / 8.464 s, and `J=20` 6.611 / 7.811 s. The first round overlapped Python tool tests. Twelve is a trial setting for this Mac, not a new global default; sustained full-build rate and memory pressure still need checking.

Further profiling of the sustained build found the Python worker at one full core. On 60 real shaders, indexing member operations once reduced reflection scanning from a median 0.824 s to 0.037 s of CPU, with identical metadata. On 100 merged sources, 300 entry lookups fell from 1.507 s to 0.046 s by finding the required literal before applying the existing entry regex. The atomic-emulation check also skips regex work when neither required literal is present. The first full attempt was stopped after about 5,600 outputs to apply these changes; the full-build effect is still unmeasured.

Cache import validation then exposed filename-sensitive output: recompiling the same merged source as `<key>_b.metal` differed from the library compiled as `<key>.metal`; keeping the basename but changing its directory matched, with and without the precompiled header. Compiles now use `shader.metal` in a private worker folder, and the convention participates in the compiler record. Fresh compiles from different input names and saved-source cache import pass. The full pass uses that convention; prior partial timings do not establish its final completion time.

The shared helper initially forced inlining, which duplicated large optimized bodies across entry points. Letting Metal choose reduced compiler CPU for 111 real pixel shaders from 141.09 / 140.56 s to 90.66 / 91.41 s; AIR size fell from 24,076,992 to 7,588,816 bytes. Wall times were 46.83 / 46.95 s versus 29.16 / 30.60 s while another build ran. All 73,728 numerical fixture components remained bit-identical. An additional heavy shader with texture reads and eleven interpolated vectors matched 524,288 components in each of six alternating trials; its median GPU time was 45.28 ms with forced inlining and 40.21 ms with compiler heuristics. This is a synthetic regression check, not a gameplay speed claim. The full pass was restarted to include this change; earlier partial builds do not establish the final build time.

Open: a later preparation profile used 64 pixel shaders from the stopped pass, the derived v327680 profile and cached Metal libraries. All 64 reflection files, 128 source files and 64 libraries matched the existing outputs byte for byte; no Metal compile was needed. Instrumented preparation used 4.93 s of thread CPU: fetch-variant analysis accounted for 1.29 s, the SPIR-V image-atomic instruction walk 0.50 s, and regex compilation 0.46 s. These are profiling costs, not an uninstrumented full-build prediction. They identify remaining preparation work to measure before choosing a process pool or further scanner changes.

Resolved preparation work, chain `7e17c769be7b`: indexing declarations reduced fetch-analysis CPU for 128 real pixel shaders from 1.832 / 1.834 s to 1.259 / 1.234 s; factoring and side-entry scanning fell from 1.660 / 1.640 s to 0.763 / 0.754 s. Outputs matched exactly. Calling the pinned SPIRV-Cross C API converted 129 real modules in 2.149 / 2.143 s versus the CLI's 2.769 / 2.913 s, with identical MSL. Separate synthetic tests cover concurrent contexts and MSL 3.1 image atomics on buffer, 2D, array and 3D images. These changes reduce the measured scans and converter launches; no process pool was added.

A combined Mac sample requested 128 keys (96 pixel, 12 compute, 8 domain, 4 hull and 8 vertex); its pixel dependency made 129 shaders, all successful. With Metal libraries already cached, the previous scanners and CLI took 9.09 s, the new path with cold intermediate caches 8.17 s, and reused intermediate stages 6.33 s; a completed-shader cache hit took 3.24 s. All 491 outputs matched after normalizing JSON key order; source and library bytes matched directly. Cache-hit reflection changed only key order in seven vertex JSON files. Reused stages avoided 129 translations and 129 source conversions. The 258 intermediate entries occupied 18,517,821 bytes versus 73,583,523 uncompressed. These are preparation/rebuild measurements, not a cold full-build estimate. The corrected chain subsequently completed the full v327680 Mac build described above. Packaged-app checks reached the opening, character creation and the first 3D scene, and the user confirmed working gameplay. A deterministic cross-revision route and full-campaign coverage remain untested.

## Rendering

- Frames take about 16.7 ms. A cold file cache, as after a reboot, keeps the game on the loading icon for over a minute, and a clean run there says nothing: check that `[nvn] frame N: … calls` reached about 3,500.
- The lit HDR target is RG11B10, exposure a 2x1 RGBA32F image, tone map `075fd7285c64b798` with a 32³ LUT, final `ed5f4c6386ca621d`, the luminance histogram compute `2118ab534fac88c6`.
- `texraw` pads rows: an RG11B10 dump has a 16-byte stride with 4-byte pixels at the head of each row. It returns one slice of a 3D texture, and serials shift between runs.

## Driving it on the Mac

The v327680 full Mac app boot on 2026-11-04 calls `nvnDeviceSetDepthMode` with near = 0. Its derived profile needs `depth_mode zero` before the offline pipeline archive is built; the default GL archive has the wrong stamp and triggers a fresh device prebuild. Both vertex depth entries remain in the full shader libraries, so correcting the archive does not require translating shaders again.


The first boot shows the language list: `A`, `LUp`, `A` picks English. The character choice takes `LLeft` before `A`. From a warm cache, `A A LLeft A A Plus` reaches walking in about 300 s. On later boots, `A` at the title screen a few times continues the save and reaches walking in about 110 s. The work folder has `run/boot.sh`, `run/hdrive.sh` (no window, timed control lines) and `run/spam.sh`. The packed app takes about a minute to reach the language list (14 s of it the prepare step on a second start), so the fixed times in `run/spam.sh` miss it: press the keys once the list shows.

## Open

- The picture has not been compared with a Switch 2. The user's playtest found it fine. Sunlit ground matches published iPhone shots of the game. Shaded ground and walls are lavender. The player carries fine contour lines, already present in colour target 5435, which may be the material. In some moving shots far blocks showed through near ones; this has not been rechecked since the fixes below.
- Only the first area has been played on this build. A new save written through the game has not been checked: the 31-minute playtest wrote none.
- The pipeline archive saved nothing on the walk from the language list to the first area, on the partial library: archive lookups found 0 and found none 655 times, and 216 bases were compiled, though the host's probe found the archive's bases. The draws' pairs seem not to be those of `shader_pairs.tsv`, which come from BNSH variations. Whether a full library differs is not checked.
- An IPA has not been built. A full iOS shader build would be needed first.

## Found on this game

- [One compile for a pixel shader's side entry points](../../../docs/reference/shaders.md#one-compile-for-a-pixel-shaders-side-entry-points): 57% of the map's compiles were `_ba` and `_bl` libraries; a cold build of 1,021 shaders now takes 48% of the CPU on the Mac's flags and 47% on the iPhone's.
- [KTGL on NVN2: Switch 2 GLSLC outputs and LD_ / LI_ LINKDATA](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata): the split found almost none of its shaders.
- [HADD2 and HMUL2 with an immediate](../../../docs/reference/shaders.md#hadd2-and-hmul2-with-an-immediate): the GPU hang and 88 ms frames.
- [Scaled shared memory addresses](../../../docs/reference/shaders.md#scaled-shared-memory-addresses): stride 3 in use, on accesses of every size.
- [HFMA2 .H0_NH1](../../../docs/reference/shaders.md#hfma2-h0_nh1).
- [Ampere instructions new in Pokémon Pokopia](../../../docs/reference/shaders.md#ampere-instructions-new-in-pokémon-pokopia).
- [A shader build that keeps its packages in memory runs one worker at a time](../../../docs/reference/shaders.md#a-shader-build-that-keeps-its-packages-in-memory-runs-one-worker-at-a-time): the full build at one or two shaders a second (resolved).
- [Each Metal compile instantiates Metal's texture classes again](../../../docs/reference/shaders.md#each-metal-compile-instantiates-metals-texture-classes-again): the full build's remaining hours.
- [A library that depends on its source's file name](../../../docs/reference/shaders.md#a-library-that-depends-on-its-sources-file-name): two `_bl` files differed between cold builds of the same code.
- [Metal compiles spend most of their time on the texture types](../../../docs/reference/shaders.md#each-metal-compile-instantiates-metals-texture-classes-again): the full build 1.6 → 3.65 shaders a second on a 10-core M5 (resolved).
- [Alpha-test and logic-operation libraries parse the same shader twice](../../../docs/reference/shaders.md#one-compile-for-a-pixel-shaders-side-entry-points): one library reduces eligible pixel shaders to one compile; measurements are in [Build performance](#build-performance) (resolved; full Mac gameplay regression open).
- [Identical shader sources race in the Metal compile cache](../../../docs/reference/shaders.md#identical-shader-sources-race-in-the-metal-compile-cache): concurrent misses share one compile (resolved; full-build savings unmeasured).
- [Shader preparation scans resource operations repeatedly](../../../docs/reference/shaders.md#shader-preparation-scans-resource-operations-repeatedly): source scans indexed or prefiltered (resolved; full-build rate pending).
- [Downstream shader changes repeat translation and source conversion](../../../docs/reference/shaders.md#downstream-shader-changes-repeat-translation-and-source-conversion): compressed stage reuse retains complete shader coverage (resolved in samples; full build open).
- [Each shader conversion launches SPIRV-Cross again](../../../docs/reference/shaders.md#each-shader-conversion-launches-spirv-cross-again): the pinned native API produces identical sampled MSL (resolved in samples; full gameplay regression open).
- [Split shader blocks retain stale predecessor links](../../../docs/reference/shaders.md#split-shader-blocks-retain-stale-predecessor-links): all 16 remaining BRX translation failures in the full v327680 pass (resolved; full gameplay regression open).
- [Metal libraries depend on the source basename](../../../docs/reference/shaders.md#a-library-that-depends-on-its-sources-file-name): stable compiler input naming restores reliable cache import (resolved).
- [Integer buffer textures read through float slots](../../../docs/reference/rendering.md#integer-buffer-textures-read-through-float-slots).
- [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer) and [GameShare guest displays](../../../docs/reference/services.md#gameshare-guest-displays): boot aborts.
- [Block-compressed storage the CPU wrote before first use](../../../docs/reference/nvn-objects.md#block-compressed-storage-the-cpu-wrote-before-first-use): its `IMAGE` BC1 / BC7 icons showed other items' pictures once their storage was reused (resolved).
