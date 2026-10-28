# Windows and Linux migration: status and handoff

This page is the handoff for the port of nxnative to Windows 10+ and Linux. It records what was decided, what exists, where it is, and what is left, so that a new session (another account, another agent) can continue without the conversation that produced it. Last updated 2026-10-28.

Read first: [AGENTS.md](../../AGENTS.md) and `CLAUDE.md` (the project rules; they apply to all of this work), then [aot-recompiler.md](aot-recompiler.md) and [vulkan-backend.md](vulkan-backend.md) (the two designs).

## Goal

Play the supported Switch games on Windows 10 or later and on Linux (run through Docker), on x86-64 PCs, as well as on the Apple platforms that work today. Targets set by the owner:

- Maximum performance, **no loss of quality or accuracy**, **no stutter**; modest PCs (an i3 with an ordinary or integrated GPU) must play. An i5 requirement is acceptable.
- Balanced waits: packing a game and starting it must not take very long. Performance is not bought with long load times.
- The Apple path (native ARM64 on Apple silicon, Metal) keeps working unchanged.
- Any decision that adds a programming language or a large dependency is presented to the owner with criteria first; the owner decides.

## Environment

- Owner's PC: Intel Core i9-13950HX (x86-64), NVIDIA GeForce RTX 4060 Laptop, Windows 11 Pro, 209 GB free on C:. Docker Desktop on WSL2, server `linux/amd64`. CMake, Python 3.13, .NET and a Chocolatey `gcc` are installed; **clang, ninja and make are not** (Windows builds are cross-compiled from Docker with llvm-mingw so nothing has to be installed).
- Only Mac available: an **Intel** work Mac. There is no Apple-silicon Mac, so `scripts/regress.py check` cannot run locally and Apple-side regressions can only be checked by macOS CI (unit tests and builds, no games) or by someone with an Apple-silicon Mac. Note that `scripts/build_ipa_from_nsp.py:2068` refuses Intel Macs today.
- Docker cannot run macOS (licence, no Metal, and it would be x86 anyway).
- The owner has six purchased games and will dump them from their own Switch (keys with Lockpick_RCM, games with nxdumptool). Game files go in a work folder outside the repository; the folder is chosen at Phase 3.
- **Warning:** `Mario-vs-Donkey-Kong.xci` (1.3 GB, a game image) is in the repository root, untracked and not ignored. AGENTS.md forbids game files in the repository. Move it to a work folder outside the repository (for example `C:\nxwork\`) and never `git add -A` while it is there. Nobody has opened or modified it.

## Git state

- Branch `multiplataforma` (created from `main` at `3e77816`, pushed to `origin`). Commit `8ed424a` ("claude") on it was made by the owner.
- Untracked on `multiplataforma`: `docs/design/` (this file and the two designs), `.claude/` (agent worktrees; do not commit).
- **Phase 0 work in progress is NOT on `multiplataforma` yet.** It is committed as unverified work in progress on the branch **`multiplataforma-fase0-wip`** (pushed to `origin`; locally also the git worktree `.claude/worktrees/agent-a3008471ce6c9b503`, branch `worktree-agent-a3008471ce6c9b503`), based on `3e77816`. When it passes its acceptance check, review its diff and bring it into `multiplataforma` (commit only when the owner asks, with `git -c user.name=seedds -c user.email=seedds@gmail.com commit`).

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
| 0 | Host portability layer in C (`dm/host/plat/`), CMake build, host unit tests on Linux (Docker) and Windows (llvm-mingw cross-build, tests run on the PC), CI jobs | No | **In progress** (see below) |
| 1 | SDL3 front end: window, audio, controllers, gyro, rumble, keyboard/touch, fullscreen and refresh matching; save/log paths | No | Not started |
| 2 | AOT recompiler, first with open-source homebrew (devkitPro/libnx) as legal input; differential tests against dynarmic | No | **Designed** ([aot-recompiler.md](aot-recompiler.md)); not started |
| 3 | Recompiler on real games, first on the Intel Mac with the existing Metal backend (isolates recompiler bugs from the Vulkan port) | Yes | Not started |
| 4 | Vulkan backend: NVN frontend/backend split, Vulkan shader target, pipelines, memory, textures, presentation | Yes | **Designed** ([vulkan-backend.md](vulkan-backend.md), all sections written; the agent was stopped before its final report, so the document has not been reviewed yet); not started |
| 5 | Full games on Windows and Linux/Docker; performance, frame pacing, P-core pinning, timers | Yes | Not started |
| 6 | Packaging for Windows (folder/zip) and Linux (AppImage/folder), launcher, CI, docs (README "not an emulator" and "iOS only" wording, AGENTS.md, development.md, games table platform column) | Yes, for the final test | Not started |

### Phase 0 detail

Brief given to the agent (keep these requirements if you continue it):

1. `dm/host/plat/plat.h` with `plat_apple.c` (existing Apple code moved verbatim, behaviour unchanged), `plat_linux.c`, `plat_win32.c`, plus a portable `sha256.c` (replaces CommonCrypto). It covers locks (`os_unfair_lock`), monotonic time (`mach_absolute_time`), thread priority/QoS, high-resolution sleep (Windows `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), virtual memory reserve/commit/protect/release/alias and page size (Windows `VirtualAlloc2`/`MapViewOfFile3`, `_WIN32_WINNT=0x0A00`, Windows 10 1803+), clonefile/xattr helpers, save/log directories (Apple unchanged, Linux XDG, Windows %APPDATA%), address-to-symbol lookup (`plat_addr_symbol`).
2. Portable host `.c` files converted. Core Audio (`hle_audio.c`) and VideoToolbox (`hle_movie_avc.c`) keep their Apple backends; other platforms get a declared null backend logging a `[RED] ` line until Phase 1.
3. Code tied to executing guest ARM64 natively stays Apple/arm64-only behind a minimal interface for the Phase 2 engine: `dm/host/entry.S`, `dm/gfx/nvn/nvn_entry.S`, `nvn_thunks.S`, `hle_bigstack.c` asm, `hle.h:45` inline stub, `tpidrro_el0` in `loader.c`, Mach thread state and signal-context parsing in `hle_thread.c`, `probe.c`, `crash.c`, Mach-O section lookups for embedded modules.
4. Apple-only front ends (`front_mac.m`, `haptics_apple.m`, `memstat.m`, `motion_pro_mac.h`, `*_apple.*`) are not ported (Phase 1).
5. `dm/host/CMakeLists.txt` (+ `dm/host/cmake/`) builds the portable host as a static library and registers the portable unit tests in CTest; it generates `game_profile.h` with `python3 dm/games/game.py header <GAME> <out>`. libvpx gets a generic x86-64 config (`dm/third_party/libvpx/config/generic/`).
6. `docker/Dockerfile.build` (Ubuntu 24.04: clang, lld, cmake, ninja, python3, git) and `docker/README.md`; Windows cross-build with a pinned `mstorsjo/llvm-mingw`.
7. CI jobs for Linux and Windows in `.github/workflows/`; macOS jobs untouched.
8. A section in `docs/development.md` linking `docker/README.md`.

State when the agent was stopped (on the owner's request): plat layer files exist (`plat.h`, `plat_apple.c`, `plat_linux.c`, `plat_win32.c`, `sha256.c`); 41 tracked files changed (+501/−318) across `dm/host/*.c`, `dm.h`, `hle.h`, both host Makefiles, `dm/gfx/tests/Makefile` (for `t_gl_imports`, which compiles `hle.c` and now needs the Apple plat layer) and many host tests; `dm/host/CMakeLists.txt`, `dm/host/cmake/` and `docker/` exist. The agent was adding a Windows shim for `posix_memalign` in `hle_audren.c` (Windows' runtime lacks it; Apple must stay identical). **Not verified yet:** nothing of this has been compiled or run anywhere; the Linux CTest run, the Windows cross-build and test run, CI jobs, docs section and the final report are not done.

Acceptance check for Phase 0 (run and paste the output):

```sh
# from the worktree root, Git Bash (MSYS_NO_PATHCONV=1, $(pwd -W) for volumes)
docker build -t nxnative-build -f docker/Dockerfile.build docker
docker run --rm -v "$(pwd -W):/src" -w /src nxnative-build sh -c \
  'cmake -S dm/host -B build/linux -G Ninja -DGAME=010015100B514000 && cmake --build build/linux && ctest --test-dir build/linux --output-on-failure'
# Windows: same CMake tree cross-built in mstorsjo/llvm-mingw for x86_64-w64-mingw32, then run the test .exe files on the PC
PYTHONUTF8=1 python -m unittest dm.tools.tests.test_docs
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

Also open: the launcher technology; ASTC textures on desktop GPUs (lossless decode offline, GPU decode at load, or BC7 recompression which loses a little); the Vulkan design's own section 12 once written; the work folder for game files (Phase 3).

## How to continue in a new session

1. Read AGENTS.md, CLAUDE.md, this page, then the two designs.
2. Check the Phase 0 worktree: `git -C .claude/worktrees/agent-a3008471ce6c9b503 status` and `diff --stat`. Finish the items listed as not done, run the acceptance check, review the full diff (Apple paths must be verbatim moves), then bring it into `multiplataforma`.
3. Review [vulkan-backend.md](vulkan-backend.md): its agent wrote every section (through 13 and Sources) but was stopped before reporting, so check sections 5–13 for completeness and for claims marked as assumptions, and collect its section 12 decisions for the owner. `PYTHONUTF8=1 python -m unittest dm.tools.tests.test_docs` passes with all three design pages.
4. Present the open decisions above to the owner, in Spanish, with criteria; wait for answers before Phase 2 and Phase 4 implementation.
5. Then Phase 1 (SDL3), Phase 2 (recompiler with homebrew), and so on. Per CLAUDE.md, delegate bounded tasks to Opus subagents with the effort table, full briefs, and verify their diffs and checks before reporting.

Working notes: the owner writes in Spanish and gets answers in Spanish; code, comments, docs and commits stay in English. The usage limit of the plan was hit several times when three agents ran in parallel; running fewer at a time is slower overall but is interrupted less.
