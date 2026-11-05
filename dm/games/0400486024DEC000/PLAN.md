# Ocarina of Time — nxnative Mac rebuild plan

This is a source-support and packaging plan for running the legally obtained Switch 2 title through nxnative on macOS. It does not recreate Nintendo's game, assets or source code. The private NUP is an input container; it, extracted modules, RomFS data, saves, logs and compiled game shaders remain outside this repository.

Status marks reflect the update v131072 package currently under survey: **done**, **in progress**, **not started**, or **blocked by evidence**. A phase is complete only after its checks pass; reaching a later phase does not imply playability.

## Immediate continuation runbook

Run these steps from the nxnative checkout. They resume safely: the shader content-addressed cache keeps completed work. Never create `inputs.tsv`, `c1_full.tsv` or `cb_extent.tsv` by hand; only a successful unfiltered shader build proves and writes them.

```sh
NX=/Users/vinicius/Documents/Switch/nxnative-oot
W=/Users/vinicius/Documents/Switch/0400486024DEC000-work-build
G=0400486024DEC000
NUP="/Users/vinicius/Documents/Switch/The Legend of Zelda Ocarina of Time [0400486024dec000][v131072] (1).nup"
PY=/opt/homebrew/bin/python3.14
cd "$NX"
```

1. **Build the observed Mac shaders only.** Pass `--seen-shaders`; without it, the script builds the full 42,854-shader map, which can take hours. The selected build covers the 2,765 shader keys in `shader_seen.tsv` plus any missing keys reported by the latest run logs; uncovered areas may render incompletely. The script owns the cache setting and stores it in `"$W/shader/cache/cas.db"`. Do not set `DMSH_CAS` to a `cas/` directory: it must be a SQLite file.

   ```sh
   DM_GAME="$G" DM_ROOT="$W" \
     "$PY" scripts/build_ipa_from_nsp.py "$NUP" \
     --work-dir "$W" --platform mac --seen-shaders --keep-extracted
   ```

   The success gate is a completed Mac build and `"$W/shader/partial-keys.txt"` containing the selected shader keys. This partial library is for development only; it is not stamped as full coverage and does not produce `c1_full.tsv` or `cb_extent.tsv`.

   ```sh
   CHAIN=$("$PY" dm/gfx/tools/dmsh_chain.py)
   test -f "$W/shader/partial/b-$CHAIN/key"
   wc -l < "$W/shader/partial-keys.txt"
   ```

   The CAS is the file `"$W/shader/cache/cas.db"`; a stale shell override pointing to a directory is ignored by the build script. If the stage fails, preserve the log, search it for `[RED]`, `failed` and `Traceback`, and rerun with the same `--seen-shaders` command.

2. **Rebuild the host and run the scoped checks.** The host and libraries must name the same chain.

   ```sh
   make -C dm/host GAME="$G" -j12 dmhost
   make -C dm/host/tests GAME="$G" build/t_codec_video build/t_net_eventfd
   dm/host/tests/build/t_codec_video
   dm/host/tests/build/t_net_eventfd
   make -C dm/gfx/tests GAME="$G" t_pool_chunks
   MTL_DEBUG_LAYER=1 dm/gfx/tests/t_pool_chunks
   "$PY" dm/gfx/tests/test_dmsh_reflect.py
   "$PY" dm/tools/tests/test_nsp_chain.py
   "$PY" dm/tools/tests/test_xlate_anycpu.py
   ```

3. **Make one clean warm-cache boot capture.** First let a cold run finish any new compute-pipeline compilation. Then run the same command again for the measured warm result; the example below captures the visible opening after the encoded-black lead-in.

   ```sh
   mkdir -p "$W/captures/full-stamp"
   DM_ROOT="$W" \
   DM_ROMFS="$W/build/extract/v131072/RomFS.img" \
   DM_MSL_DIR="$W/build/shader_lib" \
   DM_NVN_SHMAP="$NX/dm/games/$G/shader_map.tsv" \
   DM_FONT_DIR="$W/build/font" \
   DM_SAVE="$W/run/save" DM_TEMP="$W/run/temp" \
   DM_PSO_ARCHIVE="$W/run/pso_archive" \
   DM_PSO_PREBUILD=0 DM_PSO_PREPARE=0 \
   DM_NVN_SHOT="240,300:$W/captures/full-stamp/frame_%d.png" \
     dm/host/dmhost "$W/data/patched_v131072" \
     > "$W/logs/boot-full-stamp.log" 2>&1 &
   game_pid=$!
   sleep 15
   kill -TERM "$game_pid"
   wait "$game_pid"
   ```

   Require: no missing shader key; no stale/missing stamp or constant-buffer-table red line; no raw-global-memory log overflow; the learner reports one pool and reaches prediction; screenshots 240 and 300 show the night landscape; a warm run has no pipeline compilation stall. Record frame/GPU percentiles and every remaining `[RED]` line in this note before moving on.

4. **Reach control before broadening shader work.** Use an empty copy of the save, play through the complete opening, record input with `DM_REC`, and identify the first controllable frame. Replay the recording with `DM_KEYS`; add screenshots before and after the title transition and at the first stable controllable scene. Run `DM_NVN_SHDUMP` on that route and regenerate `shader_seen.tsv` only from keys actually requested. Any key absent from `shader_map.tsv` is a discovery bug to trace to its source container, not a reason to add a loose runtime-only shader.

5. **Turn the recording into `smoke.tsv`.** The first route must declare the save source, keys, duration, reach assertion, stable screenshots and hang timeout. Run it repeatedly from a new save and a loaded save. Only after this route has correct picture, input, audio, movie teardown and save/reload should pipeline first-use rows become the checked `pso_manifest.tsv`.

6. **Finish validation and package last.** Measure the provisional memory layout on the smoke route, run the full host/tool/doc checks and the five-game impact regression, then run the normal NUP consumer with `--platform mac`. Build `--platform mac-app` only after the smoke route passes. Do not sign, notarize, upload or redistribute the NUP, extracted assets, shader cache or app.

## 1. Freeze the selected input

- [x] **Done:** keep the original 24 GB NUP outside Git and select update v131072 over base v65536.
- [x] Record the base/program title ID `0400486024DEC000`, update title ID `0400486024DEC800`, package inventory and absence of DLC.
- [x] Verify that the selected update supplies its own ExeFS and whole RomFS.
- [ ] Record a private package hash and a round-trip verification report without publishing keys or game content.
- [ ] If another update is used, treat it as a new build: survey every module, regenerate every shader table and add a separate build row only after verification.

## 2. Keep a reproducible external work tree

- [x] **Done:** use `/Users/vinicius/Documents/Switch/0400486024DEC000-work` for extraction, survey, patched modules, logs, shader intermediates, caches, saves and the development host.
- [x] Keep the 26.2 GB RomFS image and 502 MB split shader package outside nxnative.
- [ ] Add small external run scripts for rewrite, host build, selected-shader build, boot, screenshot and smoke-route commands; each must derive paths from one work-root variable.
- [ ] Preserve the first successful logs for `nnMain`, the frame loop, the first correct frame, save/reload and the final smoke route.

## 3. Lock executable identity and rewriting

- [x] **Done:** register `main`, `subsdk0` and `sdk` with their full build IDs in `game.tsv`.
- [x] Confirm AArch64, SDK 22.2.8, Switch 2/Ounce, NVN2 231.40 and module load order.
- [x] Run the instruction census for all modules and require the rewriter to account exactly for every x18, thread-pointer, counter and SVC site.
- [x] Apply the shared SDK `AddressRegionManager` match that reduces its 1 GB node area to 62 MB; keep this derived from code rather than a title-specific fixed address.
- [ ] Re-run rewrite and module-load checks after every upstream rewriter change.

## 4. Finish the measured profile

- [x] **In progress:** the profile has identity, selected build, modules, RomFS marker, shader groups and a working provisional address-space layout.
- [x] Measure the first requested heap: `0x18a600000` bytes (6.166 GiB).
- [ ] Capture all address-region and `StandardAllocator` requests through a stable gameplay route.
- [ ] Replace provisional `mem_reserve`, `mem_alias`, `mem_stack` and `app_pool` values with the smallest measured layout that leaves the documented safety margin.
- [ ] Measure peak resident, alias and Metal memory on the target Mac; test a second memory size if available.
- [ ] Confirm the title's language inventory, display version and icon from control data if a future NUP supplies them; do not invent missing metadata.

## 5. Complete shader discovery

- [x] **In progress:** initially split 42,321 mapped stages and generate `shader_map.tsv`, compute facts, variation pairs and candidate pipeline pairs.
- [x] Record the 2,765 mapped shader keys requested through frame 86 in `shader_seen.tsv`.
- [x] Trace the 11 dumped runtime program keys not in the map: all 11 are compute shaders in `Movie/Ndve/Ndve-Nvn2-vid_seq5.dat.zs`, an `NDVE` upscaler container with an embedded `NDSR` block (22 shaders in total).
- [x] Extend the splitter for the proven source (`ndve_shaders`) and regenerate `shader_map.tsv` and `shader_facts.tsv`: 42,343 stages, 22 rows added, none removed; pairs and PSO tables unchanged.
- [ ] Run `DM_NVN_SHDUMP` through the eventual smoke route and require zero key outside the map.

## 6. Build the first selected Metal library

- [x] **In progress:** build `shader_seen.tsv` plus the latest boot keys for macOS, sharing nxnative's shader CAS in the external work tree: 3,493 selected shaders succeeded, including the 22 newly split NDVE shaders, with no translation failure.
- [x] Record the selected build result: 3,471 cache hits, 22 rebuilt shaders, 697 generated variants and 134 compiled variants; there were no undecoded binding slots or unrecognized binding kinds.
- [ ] Rebuild the host after any shader-chain change and regenerate the chain header when chain tools change.
- [ ] Run an unfiltered full build of the current 42,854-shader map when full-game coverage is required; require its stamp and generated constant-buffer tables, then verify the library loads.
- [ ] Expand the selected list from actual routes. A selected build is a development shortcut, not full-game shader coverage.
- [ ] Use selected incremental builds for newly discovered route keys; rerun the unfiltered build whenever full-game coverage is required or profile inputs / the shader chain change.

## 7. Reach the first correct frame

- [x] **In progress:** the host enters `nnMain`, mounts 187,182 RomFS entries, initializes Metal/NVN2 and renders at least frame 300.
- [x] Add the SDK 22 virtual audio-input empty-list answer and offline local eventfd semantics needed by bundled gRPC.
- [x] Accept the compute-workload priority hint on the host's single ordered Metal queue.
- [x] Boot with the selected shader library and capture the opening movie. Frames 240 and 300 show the night landscape after the movie's encoded-black lead-in; the AV1, NV12, NDVE compute and presentation path is visible end to end.
- [ ] Compare resolution, crop, origin, depth, color, texture formats and movie composition against a Switch 2 capture supplied by the owner.
- [ ] Resolve every non-missing-library `[RED]` line before declaring the picture correct.

## 8. Validate platform systems

- [ ] Verify the `prepo:u` no-op telemetry session lets the title continue past its service-resolution abort; trace its requests and check for subsequent diagnostics.
- [ ] Input: buttons, sticks, camera, Plus/Minus, controller reconnect, focus loss and optional motion.
- [ ] Audio: renderer output, channel layout, volume, underruns, focus transitions and a long session; keep microphone input absent unless the game requires it.
- [ ] Movies: AV1 playback, audio/video sync, skip controls, looping/teardown and picture-buffer release once rendering is active.
- [ ] Files and saves: fresh creation, autosave, clean shutdown, second-boot reload and interrupted-write recovery.
- [ ] Offline behavior: confirm NPLN/gRPC threads remain idle and no online service is required for single-player progression.
- [ ] Applets and UI: account selection, software keyboard, error paths, suspend/resume and Home-to-quit.

## 9. Establish a reproducible gameplay route

- [ ] From an empty save, document timed inputs to the first controllable scene.
- [ ] Continue through representative field traversal, combat, menus, inventory, transition/loading, cutscene and save points.
- [ ] Add `smoke.tsv` only when the route is deterministic and has frame/time reach assertions plus screenshots.
- [ ] Run the route repeatedly from a new save and a loaded save; classify every crash, hang, red line and visual mismatch.
- [ ] Grow `shader_seen.tsv` only from routes that were actually exercised and record their coverage in the title note.

## 10. Correctness before performance

- [ ] Verify GPU barriers, attachment feedback, bindless resources, indirect draws, mesh/task stages and compute dispatches used by the route.
- [ ] Check screenshots at stable frame numbers and use frame capture/replay for the first persistent mismatch.
- [ ] Record a provisional `pso_manifest.tsv` from real first-use order after shaders render; then verify it and per-device `pso_user.tsv` prewarm correctly.
- [ ] Measure frame p50/p95/p99, GPU time, long-frame causes, memory pressure and shader/pipeline compilation stalls.
- [ ] Optimize only after identical routes and images establish correctness; prefer general host fixes over title switches.

## 11. Package for macOS

- [ ] Run the normal NUP consumer path with `--platform mac`; verify that a clean work folder reproduces the rewritten modules, shader package and runnable host.
- [ ] Keep the NUP external. The Mac output may reference or copy user-owned game data for that user's build, but nxnative must not commit or distribute it.
- [ ] After the smoke route passes, build the double-clickable `--platform mac-app` output and verify resources, launcher settings, save location, logs and first-run behavior.
- [ ] Test the app from a path containing spaces and on a clean user account without development environment variables.
- [ ] Do not sign, notarize, upload or redistribute the app or NUP as part of this repository work.

## 12. Regression and completion gate

- [ ] Run the full host tests, graphics tests relevant to NVN/eventfd/audio/movie changes, Python tool tests and documentation/profile checks.
- [ ] Run `scripts/regress.py impact`, then `scripts/regress.py check --games` on five local games most likely to be affected by the general host changes.
- [ ] Re-run the Ocarina smoke route with an empty pipeline cache and with a warm cache.
- [ ] Update the game note with tested hardware, route, shader coverage, save behavior, memory and frame-time measurements; keep remaining gaps explicitly open.
- [ ] Add gallery/player-facing support only after Mac gameplay passes. Until then the profile is bring-up structure, not a playable-support claim.

## Definition of done

The Mac rebuild is complete only when a clean build from the private selected NUP reaches a reproducible gameplay route with the correct picture, sound and input; saves and reloads; reports no unexplained red diagnostics; fits the measured memory layout; passes the scoped regressions; and produces a self-contained Mac app for the owner. Full-game compatibility still requires broader route and shader coverage and must not be inferred from one smoke route.
