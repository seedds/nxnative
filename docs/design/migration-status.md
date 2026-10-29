# Windows and Linux migration: status and handoff

This page is the handoff for the port of nxnative to Windows 10+ and Linux. It records what was decided, what exists, where it is, and what is left, so that a new session (another account, another agent) can continue without the conversation that produced it. Last updated 2026-10-28 (evening, on the owner's Linux PC).

Read first: [AGENTS.md](../../AGENTS.md) and `CLAUDE.md` (the project rules; they apply to all of this work), then [aot-recompiler.md](aot-recompiler.md) and [vulkan-backend.md](vulkan-backend.md) (the two designs).

## Goal

Play the supported Switch games on Windows 10 or later and on Linux (checked through Docker; the games run natively), on x86-64 PCs, as well as on the Apple platforms that work today. Targets set by the owner:

- Maximum performance, **no loss of quality or accuracy**, **no stutter**; modest PCs (an i3 with an ordinary or integrated GPU) must play. An i5 requirement is acceptable.
- Balanced waits: packing a game and starting it must not take very long. Performance is not bought with long load times.
- The Apple path (native ARM64 on Apple silicon, Metal) keeps working unchanged.
- Any decision that adds a programming language or a large dependency is presented to the owner with criteria first; the owner decides.

## Environment

- **Current (since 2026-10-28, evening): the owner works on an Ubuntu Linux PC** (x86-64; Docker 29, CMake, Ninja, Make and Python 3 installed; **no clang, no .NET, no Wine**: everything compiles inside Docker). The Windows PC below was the environment of the earlier sessions (another Claude account) and is still where the Windows test executables run.
- Owner's Windows PC: Intel Core i9-13950HX (x86-64), NVIDIA GeForce RTX 4060 Laptop, Windows 11 Pro, 209 GB free on C:. Docker Desktop on WSL2, server `linux/amd64`. CMake, Python 3.13, .NET and a Chocolatey `gcc` are installed; **clang, ninja and make are not** (Windows builds are cross-compiled from Docker with llvm-mingw so nothing has to be installed).
- **Test flow when a check needs a machine the session does not have** (Windows for the test `.exe` files, a Mac for anything Apple): the agent puts the runnable thing into the repository together with a runner that writes its logs into the committed folder `logs/<platform>/` (see `logs/README.md`); the owner runs it; when it fails, the owner commits and pushes the logs to GitHub and the next session reads them from the repository and fixes the cause. A check that did not run in the session is reported as not run, never as passed.
- The only Mac is an **Intel** work Mac, managed by the owner's employer: **nothing can be tested on it**. There is no Apple-silicon Mac, so `scripts/regress.py check` cannot run and Apple-side regressions can only be checked by macOS CI (unit tests and builds, no games) or by a contributor with an Apple-silicon Mac. Note that `scripts/build_ipa_from_nsp.py:2068` refuses Intel Macs today.
- Docker cannot run macOS (licence, no Metal, and it would be x86 anyway).
- The owner has six purchased games and will dump them from their own Switch (keys with Lockpick_RCM, games with nxdumptool). Game files go in a work folder outside the repository; the folder is chosen at Phase 3.
- **Warning:** `Mario-vs-Donkey-Kong.xci` (1.3 GB, a game image) was in the repository root of the Windows checkout, untracked and not ignored. AGENTS.md forbids game files in the repository. Move it to a work folder outside the repository (for example `C:\nxwork\`) and never `git add -A` while it is there. The Linux checkout does not have it.

## Git state

- Branch `multiplataforma` (created from `main` at `3e77816`, pushed to `origin`). Commit `8ed424a` ("claude") on it was made by the owner; `54d9e4a` added the three design pages; `2c76606` the specific CLAUDE.md.
- **Phase 0 is on `multiplataforma` since merge commit `aa64ca4`** (it brought in `d2d7efb` from `multiplataforma-fase0-wip`; that branch is now history and is not worked on). After it: `eee5a51` (Phase 0 finished), `ad19723` (stream copy on Commit) and `12aa34a` (UTF-8 paths), all pushed; CI run 2 on `12aa34a` is green on the three `port.yml` jobs.
- `.claude/` (agent worktrees) is never committed. `logs/` IS committed on purpose (see Environment).

## Decisions taken

| Topic | Decision | Why |
|---|---|---|
| CPU on x86-64 | **Option D: ahead-of-time recompilation ARM64 → x86-64 at pack time**, with dynarmic as a JIT safety net only. Apple keeps native ARM64 execution | Highest performance and no translation stutter: everything is compiled before play. The games' code is ARM64 and cannot run natively on x86-64 |
| Recompiler language | **C++ against the LLVM API** (not Python/llvmlite) | Faster packing; the code it produces is equally fast either way |
| Rejected CPU options | A (ARM64-only: does not run on the owner's PC), B (dynarmic JIT only: C++, compile stutter and per-block cost), C (Ryujinx ARMeilleure: C#, a .NET runtime and GC inside the process) | D gives the best sustained performance and no translation stutter; the rewriter's existing offline analysis is a head start |
| Graphics on Windows/Linux | Vulkan (proposed, not objected to) | Native on both; SPIR-V is already produced by the shader chain |
| Window, input, gyro, rumble, audio | SDL3 (proposed, not objected to) | C, supports Windows 10 |
| AVC movies (VideoToolbox today) | FFmpeg libavcodec or openh264 (proposed) | libvpx and stb_vorbis are already portable |
| Build for Windows/Linux | CMake + clang (clang-cl or llvm-mingw); Apple keeps its Makefiles | MSVC does not take the host's GNU assembly and extensions |
| Shader stutter | Shaders already translated offline; pipelines in two phases (fast-linked with `VK_EXT_graphics_pipeline_library`, optimized in the background, identical output); `VkPipelineCache` on disk; extended dynamic state | No wrong frames, no long "compiling shaders" screen |
| Rejected GPU option | yuzu-style asynchronous shaders | They draw wrong or missing objects for some frames: a quality loss |
| Pack-time balance | ThinLTO, per-function cache keyed by hash, PGO profiles recorded once per game build and shipped, "balanced" (default) and "max" pack modes | Owner rejected long waits |
| CPU baseline | x86-64-v3 (AVX2, FMA3: Haswell / Zen 1 and later) with a possible v2 fallback | Open, see below |
| Launcher (replaces `Launcher.swift`) | **Open.** Recommended Dear ImGui (C++, already in the project with the recompiler); alternatives C + SDL3, Python/Tk | Needed in Phase 6 only |

## Phases

| Phase | Content | Needs games | Status |
|---|---|---|---|
| 0 | Host portability layer in C (`dm/host/plat/`), CMake build, host unit tests on Linux (Docker) and Windows (llvm-mingw cross-build, tests run on the PC), CI jobs | No | **Done: tested on Linux (35/35), on Windows CI (25/25, run 2 on `12aa34a`) and on macOS CI (`tests.yml` run 2 on `18bf526`: build check, host tests, sanitizers, tool tests and the iOS app all green)** (see below) |
| 1 | SDL3 front end: window, audio, controllers, gyro, rumble, keyboard/touch, fullscreen and refresh matching; save/log paths | No | **Done on Linux** (`ae8e308`: `dm/host/front_sdl.c`, SDL audio in `hle_audio.c`, SDL3 3.4.14 pinned; `t_front_sdl` 82 checks, `t_audio_sdl` 45, CTest 37/37); Windows CI 27/27 and macOS CI green on `18bf526`; `t_audio_sdl`'s thread-leak check waits for the count to settle since `82c818d` (CI run 8 caught an exiting SDL thread); real controllers, sound devices and mode switching unverified until a game runs (Phase 5) |
| 2 | AOT recompiler, first with open-source homebrew (devkitPro/libnx) as legal input; differential tests against dynarmic | No | **Designed** ([aot-recompiler.md](aot-recompiler.md)); **foundation done** (`da6f5d6`: `dm/tools/aot`, LLVM 18, vendored dynarmic, the integer / memory / branch / system / exclusive lifter at 0 mismatches over 24,550 vectors, the census; `aot.yml` green on GitHub, run 1 on `aae7dd9`); FP and SIMD running |
| 3 | Recompiler on real games, first on the Intel Mac with the existing Metal backend (isolates recompiler bugs from the Vulkan port) | Yes | Not started |
| 4 | Vulkan backend: NVN frontend/backend split, Vulkan shader target, pipelines, memory, textures, presentation | Yes (from P3 on) | **Designed and reviewed** ([vulkan-backend.md](vulkan-backend.md)); **M0, M1, M2 and M2b done** (`a041a27`, `14575fa`, `9f8ff7d`: `nvn_be.h`, then `nvn_state.c`, `nvn_gpuwrite.c`, `nvn_resolve.c`, `nvn_pools.c` with CPU tests on three OSes; M2 and M2b confirmed by macOS CI on `06615ba` and `f8ac96d`); **M4 first half done** (`1aff12a`: `nvn_refl.c` over cJSON, `t_refl_cmp` on the Mac pending); **M3 done** (`ac556ee`: `nvn_replay.c`, 28 per-operation `be_*`; macOS CI pending; the two-game pass dump deferred to a Mac); **M5 done** (`57dbda2`: `nvn_pipe.c`, `nvn_fmt_from_mtl`, every `R` row of every profile round-trips, the portable `P` row); M6 next |
| 5 | Full games on Windows and Linux/Docker; performance, frame pacing, P-core pinning, timers | Yes | Not started |
| 6 | Packaging for Windows (folder/zip) and Linux (AppImage/folder), launcher, CI, docs (README "not an emulator" and "iOS only" wording, AGENTS.md, development.md, games table platform column) | Yes, for the final test | Not started |

### Phase 0 detail

Brief given to the agent (keep these requirements if you continue it):

1. `dm/host/plat/plat.h` with `plat_apple.c` (existing Apple code moved verbatim, behaviour unchanged), `plat_linux.c`, `plat_win32.c`, plus a portable `sha256.c` (**only for Linux and Windows**; Apple keeps CommonCrypto so its digests are byte-identical; the review of 2026-10-28 checked `sha256.c` against Python's `hashlib` on 209 lengths × 8 chunkings, 0 mismatches). Note: `os_unfair_lock` is not in `plat.h` (it is only used by the Metal backend) and memory aliasing is not either (it comes with the Vulkan backend's host port, AOT design section 5). It covers locks (`os_unfair_lock`), monotonic time (`mach_absolute_time`), thread priority/QoS, high-resolution sleep (Windows `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), virtual memory reserve/commit/protect/release/alias and page size (Windows `VirtualAlloc2`/`MapViewOfFile3`, `_WIN32_WINNT=0x0A00`, Windows 10 1803+), clonefile/xattr helpers, save/log directories (Apple unchanged, Linux XDG, Windows %APPDATA%), address-to-symbol lookup (`plat_addr_symbol`).
2. Portable host `.c` files converted. Core Audio (`hle_audio.c`) and VideoToolbox (`hle_movie_avc.c`) keep their Apple backends; other platforms get a declared null backend logging a `[RED] ` line until Phase 1.
3. Code tied to executing guest ARM64 natively stays Apple/arm64-only behind a minimal interface for the Phase 2 engine: `dm/host/entry.S`, `dm/gfx/nvn/nvn_entry.S`, `nvn_thunks.S`, `hle_bigstack.c` asm, `hle.h:45` inline stub, `tpidrro_el0` in `loader.c`, Mach thread state and signal-context parsing in `hle_thread.c`, `probe.c`, `crash.c`, Mach-O section lookups for embedded modules.
4. Apple-only front ends (`front_mac.m`, `haptics_apple.m`, `memstat.m`, `motion_pro_mac.h`, `*_apple.*`) are not ported (Phase 1).
5. `dm/host/CMakeLists.txt` (+ `dm/host/cmake/`) builds the portable host as a static library and registers the portable unit tests in CTest; it generates `game_profile.h` with `python3 dm/games/game.py header <GAME> <out>`. libvpx gets a generic x86-64 config (`dm/third_party/libvpx/config/generic/`).
6. `docker/Dockerfile.build` (Ubuntu 24.04: clang, lld, cmake, ninja, python3, git) and `docker/README.md`; Windows cross-build with a pinned `mstorsjo/llvm-mingw`.
7. CI jobs for Linux and Windows in `.github/workflows/`; macOS jobs untouched.
8. A section in `docs/development.md` linking `docker/README.md`.

State on 2026-10-28 (evening), after the merge and the fixes made on the Linux PC:

- **Linux: done.** `docker build` of `docker/Dockerfile.build` (Ubuntu 24.04 by digest, clang 18), CMake + Ninja build of `libnxhost.a`, `libnxplat.a`, `libvpx.a` and the tests, CTest **35 of 35 passing** (12.9 s), repeated twice from a clean `build/`. `dm/host/tests/run_host_tests.sh build/linux` writes `logs/linux/`.
- **Windows: cross-built and run on CI.** The same tree cross-built in `mstorsjo/llvm-mingw:20261020` (digest in `docker/README.md`; the toolchain file must be given as an absolute path, `/src/dm/host/cmake/x86_64-w64-mingw32.cmake`): 184 build steps, 23 test `.exe` files that import only `KERNEL32`, `bcrypt` and the UCRT. **The owner runs them on the Windows PC with `dm/host/tests/run_win64_tests.ps1`** (PowerShell 5.1 or 7; logs to `logs/win64/`; the script was only exercised under pwsh 7 on Linux, so its Windows-only parts, `cmd /d /s /c` quoting, `SetErrorMode`, `taskkill`, the registry read, are unverified). The CI job `win64-test` also runs them on a `windows-latest` runner.
- **Review of the diff (independent agent, all 176 hunks):** no change breaks Apple on a normal path. Its findings were fixed: `plat_data_dir` had become a cached static buffer with a first-call race, now `plat_data_path(buf, n, rel)` writes into the caller's buffer and on Apple is the old `snprintf` byte for byte; the directory-type test in `hle_fs.c` is again `d_type == DT_DIR` on Apple (the `lstat` fallback exists only on Linux and Windows); `t_symbols.c` again asserts the `__DATA` segment on Apple; `DM_HLECOUNT` and `DM_FD_LOG` log one `[RED] ` line where they are not supported; `scripts/build_ipa_from_nsp.py` and `dm/tools/usage.py` now include `dm/host/plat` in the host fingerprint (they did not, so a Mac host build would have reused a stale `dmhost` after a plat change). Known, accepted differences on Apple: log text only (`?` for an unknown import symbol, `0+0` in one EFAULT line), float rounding of the thread dump's CPU time, and `DM_LOADHASH` of modules of 4 GiB or more. One iOS item is unverified: `hle_count.c` now sees `TARGET_OS_IPHONE` through `plat.h`; the old file may not have had it defined, so iOS may have compiled the counting code before. Check with `xcrun -sdk iphoneos clang -E -dM` on a Mac.
- **First Windows run (CI, `windows-latest`, 2026-10-28 evening): 24 of 25 tests passed.** `t_fs_case` failed two checks: on Commit, `plat_clone_tree` lost the guest-name stream of an alias directory (`CopyFileW` on a directory's stream path does not reproduce it, and the failure was ignored); `copy_streams` now reads and writes each named stream and a failure fails the clone. The same investigation found that non-ASCII paths took two conversions on Windows (UTF-8 in `plat_win32.c`, the ANSI code page in the C runtime): every Windows executable now carries `dm/host/plat/win32_utf8.manifest` (`activeCodePage` UTF-8, Windows 10 1903+), with `setlocale(LC_CTYPE, ".UTF8")` as the fallback on 1803/1809 (where `getenv` still returns the ANSI conversion: open), and `t_fs_case` has eight new checks with a non-ASCII directory and a name outside code page 1252 (54 checks on Linux). Both fixes are cross-built and pass on Linux.
- **Second Windows run (CI run 2 on `12aa34a`, 2026-10-28): all three jobs green**, `linux-host`, `win64-cross` and `win64-test` (25 of 25 tests on `windows-latest`). Phase 0 is verified on Windows as far as the unit tests reach; the owner's own PC run with `run_win64_tests.ps1` is still a useful second check of the `cmd` / `taskkill` / registry parts of the script on a non-runner machine, but nothing blocks on it.
- **Other fixes:** `t_fs_case` assumed a case-folding file system (APFS); on ext4 it now detects the file system and skips the five alias-metadata checks with a message (they still run on APFS and NTFS). `t_audren_work` builds on Windows through `sys_memalign` / `work_release` hooks. The `posix_memalign` shim of `hle_audren.c` is finished (Windows: `_aligned_malloc` / `_aligned_free`; Apple and Linux unchanged).
- **Not compiled on macOS or iOS:** none of the Apple-visible edits. macOS CI (`.github/workflows/tests.yml`) must confirm; `scripts/regress.py check` cannot run anywhere available. Also not built here: the TSan / ASan targets (the Docker image has no sanitizer runtime), the ten `fork()` death tests on Windows (listed as not built in `build/win64/host_tests.tsv`).
- **CI:** `.github/workflows/port.yml` adds `linux-host` (Docker build + CTest + `logs/linux` artifact), `win64-cross` (llvm-mingw cross-build, `.exe` artifact) and `win64-test` (`windows-latest`, runs `run_win64_tests.ps1`, `logs/win64` artifact). The macOS workflow `tests.yml` runs on pushes to `multiplataforma` too since `1fe3a1d`: it is the only place where the port's Apple-side edits are compiled and unit-tested (its `Tool tests` job failed on `main` because `test_nro_header.py` imported a sibling test without the folder on `sys.path`; fixed in the same commit).
- **Docs:** `docker/README.md` (commands from Linux and from Git Bash, image tags and digests, the log-commit flow, what is not portable), `logs/README.md`, a section in `docs/development.md`.

Acceptance check for Phase 0 (run and paste the output):

```sh
# from the repository root on Linux (docker/README.md has the Git Bash form: MSYS_NO_PATHCONV=1, $(pwd -W) for volumes)
docker build -t nxnative-build -f docker/Dockerfile.build docker
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh -c \
  'cmake -S dm/host -B build/linux -G Ninja -DGAME=010015100B514000 && cmake --build build/linux && ctest --test-dir build/linux --output-on-failure'
# result on 2026-10-28: 100% tests passed, 0 tests failed out of 35
# Windows: the cross-build command of docker/README.md (mstorsjo/llvm-mingw, absolute toolchain path), then on the PC:
#   powershell -ExecutionPolicy Bypass -File dm/host/tests/run_win64_tests.ps1 build/win64   -> logs/win64/summary.txt (commit it when it fails)
python3 -m unittest dm/tools/tests/test_docs.py
```

Also required in the report: the list of host tests not portable yet and why, every Apple-side edit with why it preserves behaviour (to be confirmed on macOS CI), and the execution-engine boundary left for Phase 2.

## Findings from the designs that change the plan

From [aot-recompiler.md](aot-recompiler.md):

- The recompiler takes the **original** NSO/NRO plus the game-behaviour patches (cuts, function-matched cuts, observation gates, frame-step stubs, spin regions) through a new `patch_nso.py --aot` mode; the Apple-only rewrites (x18, KGATE gates, SP alignment, red-zone stubs) are dropped.
- Whole-module discovery and jump-table recovery are done in C++: the Python rewriter is x18-only and too slow at this scale (Monster Hunter Rise's `main` 5.5 min, FINAL FANTASY RESONANCE 13 min at 2.7 GB), and large Unreal modules lack unwind entries.
- Two stacks: guest code sees only the guest stack and original return addresses (C++ exceptions, IL2CPP unwinder, crash reports depend on it); x86 `call`/`ret` use a native stack.
- Guest addresses one-to-one in an arena below 2^39; Windows uses placeholders with `MapViewOfFile3`.
- Instruction semantics from dynarmic's A64 decoder/translator retargeted to LLVM IR (maintained fork `azahar-emu/dynarmic`, 0BSD, needs Boost; the original repository URL is gone).
- One OS-native library per pack (DLL/.so/.dylib).
- The host has many ARM64/Apple ties to replace (assembly entries, Mach calls, guest functions called as C function pointers, 700+ forced take-overs and get-proc-address results needing thunks; the main thread's guest code runs on the host stack today).
- The iPhone address-space cuts are keyed by build ID without a platform tag (`dm/tools/sdk_patches.py:208-218`), so x86 needs new metadata to drop them.
- Possible existing Apple bug, unverified: `patch_nso.py:532-541` rewrites `cntpct`/`cntvct` reads to 19.2 MHz but not `mrs cntfrq_el0`.

### Review of the Vulkan design (2026-10-28)

[vulkan-backend.md](vulkan-backend.md) was reviewed on the Linux PC against the repository (over 60 `[V]` claims checked; three corrected in the text: the counts of the replay interface, and "121 texture reads" instead of "121 shaders" for Monster Hunter Rise's fully dynamic handles; a Review note sits under its Status line). Sections 5 to 13 are complete. What the review found, to resolve before Phase 4:

- **Inconsistencies:** level D of section 3.6 ("none of the extensions") contradicts the Vulkan 1.3 floor of 9.1 (extended dynamic state 1 and 2 are core in 1.3); the checks of 1.7, 10.5 and 11.1 need an Apple-silicon Mac that nobody has (D13); summary item 7 and D1 disagree on whether ASTC is decoded at upload or re-encoded offline; 9.1 accepts losing transform-feedback effects on devices without the extension, against the zero-loss goal.
- **Mechanisms without a phase:** the vsync fallbacks (DXGI `WaitForVBlank`, Linux presentation feedback), the GPU deswizzle in GLSL, the per-vendor quirk table, `be_guest_map` / `be_guest_unmap` with coalesced imports, `--platform windows/linux` packing, the `regress.py` Vulkan mode, `gfx_compare.py`.
- **Acceptance checks to tighten:** P3's FLIP threshold is calibrated partly from the thing under test; P4 ("p99 within one vsync") still allows one dropped frame in 100; P5 bounds a draw's wait (2 ms) but not a frame; P6, P8 and P10 have no pass criterion.
- **Performance (the owner's priority):** the background link-time-optimized compile runs only when the frame step reports slack, which a loaded 4-thread i3 may never report; a shader outside the prewarm lists means a full compile during play (Kirby's map has 27,297 shaders, Monster Hunter Rise's 23,478: compiling "all libraries at load" could take minutes on an i3, so the load-time / stutter balance needs a number before P5); before P6's mirrors every vertex, index and uniform read on a discrete GPU crosses PCIe, yet P4 already demands 60 fps on five games (move the import-versus-device-local microbenchmark into P3/P4; put the per-draw driver ring in device-local host-visible memory from the start); the per-draw CPU cost (about 4,900 draws a frame in Breath of the Wild, descriptor hashing on the submit thread) must be measured on a real i3 in P3/P4, not P10; page faults through a vectored exception handler on Windows are dearer than on macOS, so measure before choosing between faults and write watching; full barriers between passes idle desktop GPUs; ASTC decode during streaming and WDDM paging at 4 to 8 GB of VRAM are stutter sources; pipeline-cache writes can hold driver locks; DWM composition and Optimus routing add a frame unless the present mode is independent flip.
- **Decision-changing assumptions, cheapest check first:** fast-link time per pipeline (a small Vulkan test on the RTX 4060 and its iGPU); whether `VK_EXT_external_memory_host` can import the AOT arena's memory kinds (`MapViewOfFile3` views and placeholders on Windows, `memfd` shared mappings on Linux), its alignment and overlap rules (`t_vk_import` before P3, agreed between both designs); `maxMemoryAllocationCount` on NVIDIA Windows (if 4,096, coalesced imports are mandatory: Breath of the Wild peaks at 4,006 pools); host coherence of imported memory; whether a discrete GPU needs mirrors at all (microbenchmark before P4); the vertex-input and sampled-image numeric type rules (spec plus validation layers on lavapipe); which GPUs have ASTC; the Intel legacy driver's level; DXGI `WaitForVBlank` as a vsync source; which iGPU the owner's laptop has (Intel ARK says the i9-13950HX has Intel UHD Graphics, Xe-LP, which would make the Intel Windows path testable at home); the target i3 model.
- **Decisions the text makes silently and section 12 does not list:** a second chain version `DMSH_CHAIN_VK` with a separate translator project (changes AGENTS.md's one-version rule); the .NET 10 runtime on pack machines; the threshold that defines "no loss of quality" (FLIP); whether "Linux through Docker" means games inside a container (needs GPU and display passthrough); the quirk-table policy; C as the backend language (only matters if VMA, C++, is chosen in D2).

## Decisions waiting for the owner

From the AOT design (section 15 there has the criteria for each):

1. Boost (needed by dynarmic): vendor it, patch it out, or system install.
2. How players get the recompiler `nxaot`: prebuilt CI binaries pinned by hash, or build LLVM themselves.
3. CMake as a new build system for first-party code.
4. iPhone address-space cuts on x86: keep (tested) or drop (closer to hardware, needs platform-tagged cuts).
5. FPSR flags: model only where a census finds reads (proposed) or always.
6. ABA-sensitive exclusive windows: compare-and-swap semantics or exact emulation (much slower).
7. Acceptable pack-time budget and default pack mode.
8. CPU baseline: x86-64-v3 only, or also v2 with slow software FMA.
9. PGO profiles in the repository or only in the player's work folder.
10. Whether patching LLVM is acceptable if no stock calling convention meets the phase 1 targets.
11. JIT in release builds on Windows/Linux, or misses made fatal with feedback only.

Also open: the launcher technology; the work folder for game files (Phase 3).

From the Vulkan design (section 12 there has the options and criteria): D1 ASTC on GPUs without it; D2 memory allocator (VMA in C++ or our own in C); D3 first start on devices without pipeline libraries; D4 spatial upscaler; D5 guest OpenGL (2 games); D6 hardware floor (Vulkan 1.3 list; Intel 6th-10th generation; NVIDIA Maxwell-Volta); D7 video memory for mirrors and resizable BAR; D8 pipeline cache size and cleanup; D9 new tools and libraries (glslang, SPIRV-Tools, volk, SDL3, plus VMA, astcenc test-only, validation layers, a lavapipe image); D10 depth 24 + stencil without the format; D11 double precision; D12 latency against smoothness; D13 who runs the Mac regressions. Plus the silent ones listed under the review above: `DMSH_CHAIN_VK`, .NET 10 on pack machines, the quality threshold, Linux through Docker.

### Recommendations prepared for the owner (2026-10-28), against the stated priority: top speed with no stutter, no loss of quality, reasonable load and pack times

| Decision | Recommendation | Why |
|---|---|---|
| AOT 1 Boost | vendor a header-only subset (bcp) pinned with the fork | smallest friction for contributors, reproducible; patching it out diverges from the fork |
| AOT 2 nxaot distribution | prebuilt CI binaries pinned by hash (as `dmsh_xlate_bin`); building from source stays possible | an hour of LLVM build before a first pack is against "balanced waits" |
| AOT 3 CMake | yes for the new C++ tool and the Linux/Windows host (Phase 0 already uses it); Apple keeps its Makefiles | LLVM's CMake package; no second build system to maintain |
| AOT 4 iPhone address-space cuts on x86 | keep them for Phases 3 and 4; revisit dropping (platform-tagged cuts) once games run | tested behaviour first; no performance effect |
| AOT 5 FPSR | model only where the census finds reads | always modelling it costs on every saturating or FP operation |
| AOT 6 ABA-sensitive windows | compare-and-swap semantics, the census lists the windows, gameplay tests watch them | exact emulation is "much slower" and would create stutter; yuzu ran on the same semantics |
| AOT 7 pack budget and default mode | balanced by default; targets: at most 3 min for Mario, 10 min for Monster Hunter Rise, 30 min for any max pack, under 1 min repack; max mode opt-in | the owner's "balanced waits"; balanced must reach 90 % of max's frame rate |
| AOT 8 CPU baseline | x86-64-v3 only (Haswell / Zen 1 and later, every i3 since 2013) | a v2 build with software FMA is slow (stutter) and doubles the builds |
| AOT 9 PGO profiles | in the repository (offsets and counts per build, no game code) | every player gets the fast pack; nothing copyrighted |
| AOT 10 patches to LLVM | no, unless phase 1 measurements show no stock convention meets the targets; then decide with the numbers | an LLVM fork is a permanent cost |
| AOT 11 JIT in release builds | included, with the miss feedback file | a miss becomes a logged slowdown instead of a crash; the antivirus risk is manageable (one executable region) |
| Launcher | Dear ImGui (C++, already in the project with the recompiler) | one toolkit for Windows and Linux, small |
| D1 ASTC | GPU decode to RGBA8 (sRGB to sRGB8) at upload by default; cached BC7 as an opt-in low-VRAM mode chosen through `VK_EXT_memory_budget` | lossless for 8-bit LDR content, no load-time cost; BC7 is a second lossy generation |
| D2 allocator | VMA behind a C wrapper | mature budgets and dedicated allocations; C++ is already accepted for the recompiler |
| D3 first start without pipeline libraries | prewarm the seed and per-device lists in the prepare step; offer the full prebuild once at first start with a measured time estimate; decide the default after timing it on an i3 in P5 | the stutter / load-time trade-off needs a number |
| D4 upscaler | none by default; FSR 1 opt-in later if wanted | fidelity to the game's picture |
| D5 guest OpenGL | re-target the GL layer to Vulkan (option 1); the host's GL driver only as a stopgap | offline, deterministic, no driver GLSL compile stutter |
| D6 hardware floor | Vulkan 1.3 with the 9.1 list, transform feedback required; NVIDIA Maxwell-Volta supported (580 branch has pipeline libraries); Intel 6th-10th generation best effort until a real one is measured | reach without accepting lost effects |
| D7 mirrors and resizable BAR | on by default on discrete GPUs, capped at a fraction of the memory budget, only if P6 shows a gain; off on iGPUs | speed where it pays, memory left for textures |
| D8 pipeline cache | one file per game, driver and chain version; stale files deleted at start; a generous cap, no LRU at first; written at idle priority | simple and fast starts |
| D9 tools | glslang offline (SPIR-V committed), `spirv-val` only, volk, SDL3; plus VMA (D2), astcenc test-only, the validation layers, a pinned lavapipe image | small, permissive licences; volk saves a call per command |
| D10 D24S8 | D32F_S8 with `VK_EXT_depth_bias_control`; else D32F with the bias converted and a `[RED] ` line; never refuse the device | AMD lacks D24S8 |
| D11 FP64 | native where `shaderFloat64`, the lowered variant elsewhere (Intel Xe has no FP64; consumer NVIDIA runs it at 1/64 rate, so measure) | exactness where it is free |
| D12 latency | FIFO by default; the swapchain image count from the game's own NVN window texture count; MAILBOX opt-in | smooth 30 and 60 fps pacing first |
| D13 Mac regressions | macOS arm64 CI for builds and unit tests (no games); a contributor or a rented Apple-silicon Mac for the five-game `regress.py` gates and the FLIP reference captures | the owner has no usable Mac; without one, P1 and the image comparison have no acceptance path |
| `DMSH_CHAIN_VK` | yes, a second chain version with a separate translator project; AGENTS.md's one-version rule becomes one version per target | keeps every Metal library untouched |
| .NET 10 on pack machines | required (the chain already needs it); no self-contained publish (it would change the lock fingerprints) | reproducible chain |
| Quality threshold | FLIP calibrated from two Metal runs of the same route (the noise floor); structural differences are bugs, vendor floating-point differences are reported | makes "no loss of quality" measurable |
| Linux through Docker | answered: only a way to check Linux; games are packaged natively (Phase 6) | the Goal section said "run through Docker" |

### The owner's answers (2026-10-28)

Every recommendation of the table above is adopted, with these changes:

- **Pack modes (AOT 7):** both modes exist and the player chooses: `balanced` (the default) and `max`, selectable in the launcher, so a game can be packed faster or made to run faster at will. The targets of the table stay as the budget of each mode. (What "pack time" is: the one-time conversion of a game for the PC, which recompiles its ARM64 code to x86-64 and translates its shaders; a longer pack buys a faster game. Nothing is compiled while playing in either mode.)
- **D8 pipeline cache:** both a size limit and an automatic cleanup exist, each one switchable in the launcher's panel (on or off), with the defaults of the table.
- **D13 Mac regressions:** left for last. The owner has no usable Mac; macOS CI covers builds and unit tests. The five-game `regress.py` gates and the Metal reference captures stay open until a Mac is found; they are not needed for the Windows and Linux work itself, only to prove that the Apple path is unchanged on real games and to compare pictures.
- **.NET 10:** not accepted as a requirement on players' machines. The shader translator (`dmsh_xlate`, Ryujinx's translator in C#) needs a .NET runtime today; the chosen way is a self-contained (or NativeAOT) build of the translator per OS, produced by CI and pinned by hash in the chain lock beside the macOS launcher it already records, so that packing on a PC needs no .NET install. The framework-dependent assemblies stay for developers. This is a chain change (`dmsh_chain.py`, `dmsh_chain.lock`, `test_nsp_chain.py`) to plan in Phase 1 or P0 of the Vulkan plan.
- **Linux through Docker:** it only meant checking that the host works on Linux; no game runs inside a container. The goal is native Linux packaging (Phase 6).

## The program of the port (started 2026-10-28 evening, on the owner's instruction to run every phase without further questions)

The owner delegated every remaining decision to the agent, to be taken as the owner thinks: top speed with no stutter, no loss of quality, reasonable load and pack times, nothing to install for players. Work runs in waves of two Opus 5.5 subagents (CLAUDE.md), each in a worktree, each verified by the lead before its commit; the owner pushes and the three workflows (`port.yml`, `tests.yml`, `aot.yml`) check what this machine cannot (Windows execution, the Apple side).

| Wave | Track A (CPU side) | Track B (graphics side) | State |
|---|---|---|---|
| 1 | Phase 1: SDL3 front end (`dm/host/front_sdl.c`, SDL audio in `hle_audio.c`, headless tests with SDL's dummy drivers; `ae8e308`) | Vulkan M0 + M1 (`a041a27`) | done; `port` run 5 and `tests` run 2 green on `18bf526` |
| 2 | Phase 2 foundation: `dm/tools/aot` (CMake, LLVM 18, vendored `azahar-emu/dynarmic` at `a466015`, Boost from the image), the lifter for integer / memory / branch / system / exclusive classes, the differential vector harness against dynarmic, the census subcommand, `docker/Dockerfile.aot`, `aot.yml` (`da6f5d6`) | Vulkan M2: NVN state and GPU-write ledger into pure C files with CPU tests on three OSes (`14575fa`, macOS CI green on `06615ba`); M2b: resolve and pools (`9f8ff7d`, macOS CI green on `f8ac96d`) | done |
| 3 | FP and SIMD lowering (phase 2 of the AOT plan; running), the HLE thunk generator and the host-function page (design 2.4; `170c233`), then NRO loader + discovery + the homebrew corpus in `devkitpro/devkita64` (phase 1 of the AOT plan completed) | M4 first half (`1aff12a`), M3 (`ac556ee`), M5 (`57dbda2`) | done, macOS CI pending for M3 and M4 |
| 4 | Control flow, safepoints, JIT fallback (phase 3 of the AOT plan); host x86 integration (thunks, `dm_guest_call`, arena) | M6 (build split), P2 (Vulkan shader chain with .NET in Docker), P3 (Vulkan core on lavapipe, headless) | planned |
| 5 | Phases 3 and 5 need the owner's game dumps: prepared (packing `--platform`, launcher, CI) and tested when the games arrive | P4 and later on the owner's RTX 4060 | needs games |

Decisions taken by the lead inside this program (recorded here so they are not re-decided): Boost comes from the Docker image (`libboost-dev` 1.83 in `Dockerfile.aot`): the `bcp` subset of the four headers dynarmic includes is 21 MB, over the 15 MB allowed (revisit when `nxaot` binaries are distributed); the dynarmic issues the vectors found (`dm/tools/aot/README.md`: `msr nzcv` forwarding in its get/set elimination pass, `dc zva` at unaligned addresses, FPSR after a block lookup, 56-bit branch targets, hints as exceptions) are recorded, not patched, until the JIT fallback task decides per issue (the vendored copy is unchanged); FPCR keeps dynarmic's mask until the A57 TRM is checked; LSE atomics and top-byte-ignore are answered by the census on real games (none here) before any lowering; `nvn_be.h` keeps one `be_*` per `mtl_*` statistics call until a Mac can compare logs (`be_report` later); the chain's `--check-tools` stays Mac-only until the next chain bump (a portable `dmsh_chain.py` would bump `DMSH_CHAIN` and force every Metal rebuild); the SDL front end's progress screen and text input have no fonts until the Vulkan presenter draws them.

## How to continue in a new session

1. Read AGENTS.md, CLAUDE.md, this page, then the two designs (the Vulkan one has a Review note under its Status line).
2. Phases 0 and 1 are on `multiplataforma`, green on the three CI workflows. Still open: the iOS `TARGET_OS_IPHONE` question in `hle_count.c` (compiles; behaviour unverified), the sanitizer targets on Linux (the Docker image has no sanitizer runtime).
3. The decisions are taken (the owner's answers above). Phase 2 and Phase 4 can be planned; Phase 1 (SDL3) first.
4. Before Phase 4, resolve the Vulkan review items (the level D definition, the phases without an owner, the acceptance checks, the `VK_EXT_external_memory_host` / AOT arena question) in the design.
5. Then Phase 1 (SDL3), Phase 2 (recompiler with homebrew), and so on. Per CLAUDE.md, delegate bounded tasks to Opus 5.5 subagents with the effort table, full briefs, at most two at a time, and verify their diffs and checks before reporting.

Working notes: the owner writes in Spanish and gets answers in Spanish; code, comments, docs and commits stay in English. The usage limit of the plan was hit several times when three agents ran in parallel; two at a time is the rule now (CLAUDE.md). On the Linux PC the Python tool tests that need macOS tools or the `capstone` / `lz4` modules fail before and after any change (344 tests: 3 failures, 38 errors, 54 skipped at `aa64ca4` and after Phase 0 alike); a venv on the data drive with those two modules would make most of them run.
