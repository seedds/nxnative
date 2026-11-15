# The host on Linux and Windows: build and test in Docker

This folder holds the build environment of the portable host for Linux and Windows (x86-64), Phases 0 and 1 of the port in [migration-status.md](../docs/design/migration-status.md). What builds today is the host's platform layer (`dm/host/plat/`), the host sources that do not depend on running the guest's ARM64 code natively, the SDL3 front end and audio output (below), libvpx, SDL3 itself, and the host unit tests that need neither native execution nor an Apple front end. No game runs on Linux or Windows yet. The Apple builds do not use any of this: they keep `make -C dm/host` and `make -C dm/host/tests`.

The build is [dm/host/CMakeLists.txt](../dm/host/CMakeLists.txt). It needs a game profile (`-DGAME=<base game title ID>`, as the host Makefile does; the commands below use Mario's, `010015100B514000`). Build trees go to `build/` at the repository root, which git ignores. Each run of the tests writes its logs to `logs/<platform>/` ([logs/README.md](../logs/README.md)).

## Images

| Use | Image | Contents |
|---|---|---|
| Linux build and tests | `nxnative-build`, built from [Dockerfile.build](Dockerfile.build) | Ubuntu 24.04 pinned by digest; clang and lld 18.1.3, CMake 3.28, Ninja, Python 3.12, git, and the development headers SDL3's Linux drivers compile against (X11, Wayland, libdecor, xkbcommon, EGL, ALSA, PulseAudio, PipeWire, D-Bus, udev, IBus), Vulkan without a GPU ([below](#vulkan)): Mesa's lavapipe, the Vulkan loader, the validation layer, glslang and SPIRV-Tools, what the engine's JIT fallback needs ([below](#the-jit-fallback)): Boost's headers and clang++ 18, and DejaVu Sans (`fonts-dejavu-core`), the system font the port's host gives a game as its standard shared font |
| Windows cross-build | `mstorsjo/llvm-mingw:20261020` (digest `sha256:dadd757c364edee190c53d8e71a135892a6986182e873205d3611f85e26f290b`) | llvm-mingw: clang 23.1.4 for `x86_64-w64-mingw32` with the UCRT and winpthreads, CMake 3.28.3, Ninja 1.11.1, Python 3.12.3, git; no Boost: the JIT fallback takes the headers of [dm/third_party/boost](../dm/third_party/boost/VENDOR.md) ([below](#the-jit-fallback)) |
| Shader chain for Vulkan ([below](#the-shader-chain-for-vulkan)) and the port's prebuilt tools ([below](#the-ports-tools)) | `nxnative-chain`, built from [Dockerfile.chain](Dockerfile.chain) | the .NET SDK 10.0.404 image (`mcr.microsoft.com/dotnet/sdk:10.0.404-noble`, Ubuntu 24.04) pinned by digest; Python 3.12 with PyYAML, the SPIR-V tools of `dmsh_chain_vk.lock` (SPIRV-Tools, glslang, SPIRV-Cross) built from pinned source by [build_spirv_tools.sh](build_spirv_tools.sh) in a stage of its own, fontTools and DejaVu Sans for the packs' button-icon font, and clang 18 for Native AOT |
| The Windows tests under Wine ([below](#running-the-windows-tests-under-wine)): not Windows | `nxnative-wine`, built from [Dockerfile.wine](Dockerfile.wine) | Ubuntu 24.04 pinned by digest; its Wine 9.0 (`9.0~repack-4build3`), 64-bit only, without Mono, Gecko or fonts, its PE DLLs stripped of their debug sections; Xvfb and the X11 libraries Wine's X11 driver loads; Mesa's lavapipe (25.2.8) and the Vulkan loader; Python 3.12 and git. About 1.07 GB |

The Linux image takes about 1.8 GB on disk (the SDL3 headers are a layer of their own, about 340 MB, the Vulkan packages another, about 190 MB, and Boost's headers a third, about 160 MB); the llvm-mingw image about 1.6 GB (350 MB to download: it carries toolchains for four architectures). The Windows programs it makes are linked statically (`-static`, SDL3 and libc++ included): they need only the system's own DLLs (`KERNEL32.dll`, `bcrypt.dll` and the UCRT, which every Windows 10 has; the programs with SDL3 also `USER32`, `GDI32`, `IMM32`, `ole32`, `OLEAUT32`, `SETUPAPI`, `SHELL32`, `VERSION`, `WINMM` and `ADVAPI32`), so the test `.exe` files run on a stock Windows 10 or 11. The JIT fallback (dynarmic and its externals, linked into `nxhost-run.exe` and the AOT tests) adds no DLL to that set: on 2026-11-14 the 84 programs imported 26 DLLs in all with it as without it (`-DNXHOST_JIT=OFF`), and the four that link it import one more of the UCRT's, `api-ms-win-crt-multibyte-l1-1-0.dll` (`_mbtowc_l`, for libc++'s locale code), which `nxlauncher.exe` imports anyway. To check a program: `llvm-readobj --coff-imports build/win64/t_front_sdl.exe | grep -i '\.dll'` in the llvm-mingw image.

## SDL3

The front end on Linux and Windows (`dm/host/front_sdl.c`: window, keyboard, mouse as touch, controllers with gyro and rumble, fullscreen with refresh-rate matching, the progress screen, the text box, the save-failure notice) and the audio output there (`dm/host/hle_audio.c`) use SDL3, which [dm/host/CMakeLists.txt](../dm/host/CMakeLists.txt) downloads and builds as a static library at configure time (CMake `FetchContent`):

| Version | Tag | Commit | Released |
|---|---|---|---|
| SDL 3.4.14 | `release-3.4.14` | `147a8ee32dbf9ac02f3794964490687b6bbda1bc` | 2026-08-03 |

It is the newest SDL3 release that was at least two months old when it was chosen (2026-10-28; 3.4.16 and 3.4.18 came out in September and October): a stable bugfix release with time for regressions to be reported. The configure step makes a shallow clone of the tag and stops when its commit is not the one above (a moved tag). Only what the host uses is built: video (X11, Wayland, Windows, the dummy driver), the software renderer (the progress screen), Vulkan surfaces, events, joystick and gamepad (HIDAPI, which drives the PlayStation, Switch and Xbox controllers' gyro and rumble, and the virtual joysticks the tests use), haptic, sensor and audio (PipeWire, PulseAudio, ALSA, WASAPI, DirectSound, dummy). On Linux SDL loads X11, Wayland and the sound servers with `dlopen` at run time, so a program built here starts on a PC without one of them (that driver is skipped); the Linux test programs link only `libc` and `libm`.

Offline, or to build from a copy you already have, point CMake at an unpacked copy of that release: `-DFETCHCONTENT_SOURCE_DIR_SDL3=/src/<path to the copy>` (a path inside the container). Nothing is downloaded then; the commit is checked when the copy is a clone, and the configure step says so when it is not (it must be 3.4.14).

The SDL tests run on SDL's dummy drivers, which they choose themselves (`SDL_VIDEO_DRIVER` and `SDL_AUDIO_DRIVER` set to `dummy` through SDL's hints before SDL starts), so they need no display, sound card or controller, on Linux in Docker as on a Windows runner: `t_front_sdl` (keys, mouse, focus, virtual gamepads as players with buttons, sticks, triggers, motion and rumble, the progress screen, the save-failure notice, the text box) and `t_audio_sdl` (an output through `OpenAudioOut` on the dummy device: callbacks, released buffers, Stop, suspend, Close, no thread left behind).

## Vulkan

The Vulkan backend's tests (`t_vk_device`, `t_vk_import`, the GPU tests `t_vk_*` and the presenter's `t_vk_present`: [dm/gfx/hostvk](../dm/gfx/hostvk/README.md#tests)) run headless on Mesa's software Vulkan driver, lavapipe, under the Khronos validation layer, installed by a layer of the Linux image of its own. The image is pinned by the digest of its base, but apt resolves the packages from Ubuntu 24.04's archive when the image is built; the build prints the versions (`dpkg-query` at the end of that layer). On 2026-10-29 they were:

| Package | Version | What |
|---|---|---|
| `mesa-vulkan-drivers` | 25.2.8-0ubuntu0.24.04.4 | lavapipe (`libvulkan_lvp.so`, manifest `/usr/share/vulkan/icd.d/lvp_icd.json`, Vulkan 1.4.318), and Mesa's hardware drivers, which find no GPU in the container |
| `libllvm20` | 1:20.1.2-0ubuntu1~24.04.3 | lavapipe's code generator (already in the image: Mesa's EGL in the SDL3 layer needs it) |
| `libvulkan1` | 1.3.275.0-1build1 | the Vulkan loader, which volk loads at run time (already in the image; no `libvulkan-dev`: nothing links against it) |
| `vulkan-validationlayers` | 1.3.275.0-1 | `VK_LAYER_KHRONOS_validation` |
| `glslang-tools` | 15.1.0-2~ubuntu0.24.04.2 | `glslangValidator`, for `dm/gfx/hostvk/tests/gen_spirv.sh` |
| `spirv-tools` | 2025.1~rc1-1~ubuntu0.24.04.2 | `spirv-val`, the same script's check |

Two variables choose them: `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` (with `VK_LOADER_DRIVERS_SELECT=lvp*`, which this loader also reads) makes lavapipe the only driver, and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` turns the validation layer on. CTest sets them for those tests ([dm/host/CMakeLists.txt](../dm/host/CMakeLists.txt), `HOSTVK_ENV` through the `ENV` option of `host_test`; the descriptor microbenchmark `t_vk_descbench` gets the driver variables only, since it would measure the layer), and then a missing driver or layer fails them. The runners do not set them; the tests still run on lavapipe there (the only driver with a device in the container), with the layer they turn on themselves. Ubuntu 24.04's loader is 1.3.275, so the device, which reports Vulkan 1.4.318, is used at Vulkan 1.3. The manifest is `lvp_icd.json` in this Mesa, not the `lvp_icd.x86_64.json` of older packages. The Windows test programs import no Vulkan DLL (volk loads `vulkan-1.dll` at run time): on a PC or runner without a Vulkan loader or driver they print `note: no Vulkan device: skipped` and pass.

## The JIT fallback

The x86-64 engine's JIT fallback (dynarmic, for guest code a pack has no compiled entry for; `NXHOST_JIT` in [dm/host/CMakeLists.txt](../dm/host/CMakeLists.txt)) needs a C++20 compiler and Boost's headers (dynarmic includes `boost/icl` and `boost/variant`). The Linux image has both in a layer of its own at the end of its toolchain layers, so the layers above stay cached: `libboost-dev`, as the recompiler's image has it ([Dockerfile.aot](Dockerfile.aot)), and clang++ 18 (part of the image's `clang-18` package) made the `c++` and `clang++` CMake finds, with `CXX=clang++`. Where CMake finds no Boost, the host takes the subset of Boost 1.83.0 in [dm/third_party/boost](../dm/third_party/boost/VENDOR.md) (the headers dynarmic includes and every header they include, with all of `boost/config`: 869 files, 4.5 MB, nothing to download; its `VENDOR.md` has the command that makes it and the other ways measured), and the configure line says which: `NXHOST_JIT on: the JIT fallback (dynarmic) with Boost 1.83.0 (the subset in dm/third_party/boost)`. So `NXHOST_JIT` is on by default wherever a C++ compiler is found: a tree configured in this image builds the JIT fallback in `nxhost-run` and the AOT tests with the image's Boost, and the Windows cross-build ([below](#from-a-linux-host)) with the subset (the llvm-mingw image has no Boost). Without it (`-DNXHOST_JIT=OFF`, or no C++ compiler) guest code with no compiled entry stops the process with status 3 and a `[RED] ` line; the line `# engine:` of `host_tests.tsv`, which the runners copy into `summary.txt`, says which the tree has. `-DNXHOST_JIT=ON` makes it required (the CI's Linux and Windows jobs). The layer prints the versions apt resolved; on 2026-11-01 they were:

| Package | Version | What |
|---|---|---|
| `libboost-dev` | 1.83.0.1ubuntu2 | the metapackage of Ubuntu 24.04's default Boost |
| `libboost1.83-dev` | 1.83.0-2.1ubuntu3.2 | Boost 1.83's headers (`BoostConfig.cmake`: `find_package(Boost 1.57)` finds it) |
| `clang-18` | 1:18.1.3-1ubuntu1 | already in the image: its `clang++-18` |
| `libstdc++-13-dev` | 13.3.0-6ubuntu2~24.04.1 | already in the image: the C++ standard library clang++ compiles against |

The JIT fallback runs the guest's original code, and stops at the gate sites the packs call the host at (`nxaot_rt_gate`: the observation gates and the spin waits' back-edges the packs serve), where the host serves the gate as compiled code does and the JIT goes on ([dm/tools/aot/README.md, The JIT fallback](../dm/tools/aot/README.md#the-jit-fallback)); its code is every loaded module's text and aux region (the frame-step stubs). `DM_AOT_JIT_ALL=1` runs every guest instruction in it, which `t_aot_module` does for a second run and fault of its synthetic module.

On Windows the same dynarmic is compiled by llvm-mingw's clang 23 against libc++, which needed two includes dynarmic's copy lacked (fmt's `<stdlib.h>` for `malloc`, `<vector>` in `ir_emitter.h`: [dm/third_party/dynarmic/VENDOR.md](../dm/third_party/dynarmic/VENDOR.md), "Local changes"); nothing else in dynarmic or in the JIT glue differs. Its fastmem faults reach it through a function table over each instance's code cache (`RtlAddFunctionTable`, dynarmic's `exception_handler_windows.cpp`), a frame-based handler, which the system calls only after every vectored one: the host's vectored handler ([dm/host/aot_fault.c](../dm/host/aot_fault.c)) runs first, gives a write to a page the dirty tracker protects to the tracker, takes a fault of a pack's code or of an access the JIT's memory callbacks make (a guest fault, reported at its guest instruction), and passes the rest on, so dynarmic sees its own fastmem faults (`t_aot_host`'s `dirty` rows of fiber check that order: stores of JIT code reach the tracker's hook, none of them from the JIT's memory callbacks). The code cache is committed read-write-execute (`PAGE_EXECUTE_READWRITE`), as dynarmic does on Windows without its no-execute option. This part is written from the Windows documentation and not run here (no Windows machine): the PC's runner is its check ([below](#running-the-windows-tests-on-the-pc)).

## Optimisation level

The host's own C (`HOST_FLAGS` in [dm/host/CMakeLists.txt](../dm/host/CMakeLists.txt): the host, the NVN frontend, the Vulkan backend, the guest Vulkan and GL layers, the execution engine's run time and the tests) is compiled at `-O2 -g`; the vendored code keeps its own level (libvpx and SDL3 `-O2`, dynarmic `-O3`, the JIT glue `-O2`). Apple's Makefiles keep `-O1`, set with the first Mac host and never measured; nothing here changes them. `-DHOST_OPT=<flags>` (a CMake list, `"-O1;-fno-inline"`) builds another level for a measurement. The level was chosen on 2026-11-14 by measurement, clang 18.1.3 in this image, each level's tree built alone and the benchmark modes in the table run interleaved between two trees, pinned (`taskset -c 6,7`), seven processes each; the machine was shared with other work (load 6 to 9 for the first pair, 3.4 to 6.6 for the second), so medians are compared:

| Benchmark (median) | `-O1` | `-O2` | `-O3` against `-O2` |
|---|---|---|---|
| ASTC decode on the CPU, 1024 x 1024, the 14 block sizes summed (`t_astc bench`) | 805 ms | 630 ms (−22%; −19 to −26% per size) | −2% |
| CPU deswizzle 2048 x 2048 RGBA8 / RGBA16F / BC7 (`t_vk_deswizzle bench`) | 4.2 / 8.9 / 0.33 ms | 3.5 / 7.3 / 0.30 ms (−18 / −19 / −9%) | +3 to +11% |
| one second of six-channel output pulled and downmixed, volume 1.0 / 0.5 (`t_audio_sdl bench`) | 0.24 / 0.46 ms | 0.24 / 0.37 ms (0 / −20%) | ±2% |
| replay, submit thread CPU per draw: 4,000 triangles / indexed / with state changes (`t_vk_index_convert bench`) | 2.9 / 3.1 / 3.1 µs | 2.8 / 2.9 / 3.0 µs (noise) | ±6% (noise) |
| index conversion on the CPU (the fallback past the GPU path's limit) | 189 µs a 10,000-quad draw | 185 µs | −15% |
| execution engine: guest call, gates, host call, dispatcher lookups (`t_aot_module` timing lines) | 413 to 450 ns, 5.5 ns | the same | the same |
| `t_vk_descbench` record time, cached sets / push descriptors | 2.10 / 4.00 ms | 2.12 / 3.98 ms | the same |
| `nxhost-run` size | 21.40 MB | 21.51 MB | 21.74 MB |

A bench mode is an argument of the test program, not a CTest run: `taskset -c 6,7 build/linux/t_astc bench`, the Vulkan ones on lavapipe without the validation layer (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DM_VK_VALIDATION=0`). The textures' and samples' checksums the bench modes print are the same at every level, and so are the CTest totals and every test's check count, with and without `DM_AOT_ARENA_CHECK=1`. `-O3` gains only in a path games rarely take, so it is not used. The image has no sanitizer runtime, so the sanitizer builds stay the Mac's (`dm/host/tests/Makefile`); UBSan in trap mode needs none, and a tree configured with `"-DHOST_OPT=-O2;-fsanitize=undefined;-fsanitize-trap=undefined;-fno-sanitize=alignment,function"` stops three tests (a check that fails stops its test with SIGILL) at three places of undefined behaviour that do not depend on the level: a module's init address summed as a pointer past its image in `hle_ro.c`, `qsort` of a NULL index in `nvn_pools.c`, an offset from a NULL record in `t_replay.c`. Apple compiles those three files too, so their one-line fixes wait on the branch `wip/ub-fixes-shared` for a Mac run; with them that tree passes the whole CTest, plain and with `DM_AOT_ARENA_CHECK=1`.

## From a Linux host

From the repository root. The container runs as your own user, so the files it writes in `build/` and `logs/` are yours:

```sh
docker build -t nxnative-build -f docker/Dockerfile.build docker

# Linux: configure, build, run every test and write logs/linux/
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh -c \
  'cmake -S dm/host -B build/linux -G Ninja -DGAME=010015100B514000 && cmake --build build/linux && sh dm/host/tests/run_host_tests.sh build/linux'

# the same tests through CTest (no log files; --output-on-failure prints a failing test's output)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build \
  ctest --test-dir build/linux --output-on-failure

# Windows: cross-build every target into build/win64 (the tests run on a Windows PC, below)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src mstorsjo/llvm-mingw:20261020 sh -c \
  'cmake -S dm/host -B build/win64 -G Ninja -DGAME=010015100B514000 -DCMAKE_TOOLCHAIN_FILE=/src/dm/host/cmake/x86_64-w64-mingw32.cmake && cmake --build build/win64'
```

`run_host_tests.sh` runs the test programs of `build/linux/host_tests.tsv`, then the tests that are scripts, which `dm/host/CMakeLists.txt` (`host_script_test`) lists in `build/linux/host_scripts.tsv` from the same arguments it registers with CTest: `t_vk_spirv` (`gen_spirv.sh --check`) and `t_second_pack` (the Python driver `dm/host/tests/t_second_pack.py`, registered when `NXHOST_PROGRAM` and `NXHOST_LAUNCHER` are on). Each gets its own log; one that exits with 77 is reported `SKIP` with its own skip line and does not fail the run. `t_second_pack` needs `nxaot` (`-e NXAOT=<path inside the container>` on `docker run`, which the runner passes through, or the CMake option `NXAOT`) and the programs `nxhost-run` and `nxlauncher` built (`cmake --build build/linux --target all nxhost-run nxlauncher`); without `NXAOT` it takes the packs `build_packs.sh` prebuilt for it with `nxaot` (`<packs>/second_pack`, through `NXAOT_PACKS`: the option or the environment), and with neither it is a `SKIP` and `summary.txt` says `NXAOT not set` and `NXAOT_PACKS not set`.

The toolchain file is given by its absolute path inside the container: CMake looks for a relative `CMAKE_TOOLCHAIN_FILE` in the build folder first and then in `dm/host`, not in the current folder.

The same image cross-builds the recompiler for Windows, `nxaot.exe` (statically linked, it imports only Windows' own DLLs): first LLVM 18.1.3 and lld as static libraries from their source tarballs (`dm/tools/aot/tools/build_llvm_win64.sh`, about 20 minutes at `-j 4` on 8 vCPUs and 0.85 GB of work space at its peak; the prefix it prints stays, 0.2 GB), then `nxaot.exe` against them (66 MB). The commands, the figures and the check on a Windows PC (`dm\tools\aot\tests\run_win64_nxaot.ps1`, logs in `logs\win64\nxaot\`) are in [dm/tools/aot/README.md, Building nxaot for Windows](../dm/tools/aot/README.md#building-nxaot-for-windows).

## From Git Bash on Windows (Docker Desktop)

The same commands, with two changes: `MSYS_NO_PATHCONV=1`, so that Git Bash does not rewrite `/src` and the other container paths into Windows paths, and `$(pwd -W)` for the volume, which gives Docker the Windows form of the folder (`C:/...`). Leave out `--user`: Docker Desktop gives the files the container writes to your Windows user.

```sh
export MSYS_NO_PATHCONV=1
docker build -t nxnative-build -f docker/Dockerfile.build docker

docker run --rm -v "$(pwd -W):/src" -w /src nxnative-build sh -c \
  'cmake -S dm/host -B build/linux -G Ninja -DGAME=010015100B514000 && cmake --build build/linux && sh dm/host/tests/run_host_tests.sh build/linux'

docker run --rm -v "$(pwd -W):/src" -w /src mstorsjo/llvm-mingw:20261020 sh -c \
  'cmake -S dm/host -B build/win64 -G Ninja -DGAME=010015100B514000 -DCMAKE_TOOLCHAIN_FILE=/src/dm/host/cmake/x86_64-w64-mingw32.cmake && cmake --build build/win64'
```

A build folder belongs to the machine that configured it: CMake records its absolute paths. To switch between a Linux host and Git Bash on the same checkout, delete `build/linux` or `build/win64` first.

## Running the Windows tests on the PC

The short version for the PC, without Docker, is [docs/windows-tests.md](../docs/windows-tests.md).

CTest cannot run the cross-built tree: its files name the container's paths (`/src/...`). The configure step therefore writes `build/win64/host_tests.tsv`, the list of runs (name, program, arguments, whether it needs `TMPDIR`), and two runners read it. From the repository root, after the Windows cross-build:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File dm\host\tests\run_win64_tests.ps1
```

or from Git Bash:

```sh
sh dm/host/tests/run_host_tests.sh build/win64
```

Both run every test program of `build/win64` with a 120 s limit each (`-TimeoutSeconds` in PowerShell, `TIMEOUT=<seconds>` for the shell script), print `PASS`, `FAIL` or `TIMEOUT` per run and a total, then run the script tests of `host_scripts.tsv` that support Windows (`t_second_pack`, with the Python of `PYTHON` or `py -3`: it needs Python 3.12 or later, as the packer does; `t_vk_spirv` is Linux only, and `summary.txt` lists it as `not run on Windows: t_vk_spirv (<why>)`), write `logs/win64/<test>.log` and `logs/win64/summary.txt`, and exit with 0 when every run passed and 1 otherwise. The AOT tests run the Windows packs of `build\packs\win64` when that folder exists (`-Packs <folder>` names another; [The recompiled corpus in the host](#the-recompiled-corpus-in-the-host)), and `summary.txt` says which. `-ExecutionPolicy Bypass` applies to that one command only (Windows refuses unsigned scripts by default); `-BuildDir` and `-LogDir` choose other folders. With the port's prebuilt tools in `build\port_tools\win-x64` (or `-PortTools <folder>`: [The port's tools](#the-ports-tools), published on Linux or taken from a bundle's `bin\`), the PowerShell runner also runs `port_nx_extract` (`dm/tools/tests/test_nx_extract.py` with `NX_EXTRACT`: the usage, a synthetic NSP) and `port_dmsh_xlate_vk` (`dm/gfx/tests/test_dmsh_vk.py` with `XLATE_VK`: the lock's `win-x64` row, the embedded source, three shapes translated) with the same Python, as two more script tests. A test that crashes ends with an NTSTATUS exit code, which the PowerShell runner also prints in hex: `0xC0000005` is an access violation, `0xC0000409` a fast fail, which is also how `abort()` and a failed `assert` end a program built on the UCRT. The PowerShell runner sets `SEM_NOGPFAULTERRORBOX` for the tests it starts (`SetErrorMode`, which child processes inherit), so an access violation ends that test at once instead of waiting on an error dialog; a dialog that still appears (from Git Bash, or for a fast fail where Windows Error Reporting is set to show one) holds the test until its time limit.

### When a run fails: commit the logs

The logs are meant to be read by whoever fixes the failure, here or on another machine, so a failing run's logs are committed and pushed:

```sh
git add logs/win64
git commit -m "Host test logs: Windows, <n> failed"
git push
```

Each log starts with the date, the commit the run was built from (`git rev-parse HEAD`, marked when the working tree had uncommitted changes) and the exact command; `summary.txt` adds the Windows version, the runner and the toolchain that built the tree. [logs/README.md](../logs/README.md) has the convention. The Linux runs in Docker write `logs/linux/` the same way.

## Running the Windows tests under Wine

The closest check of the Windows programs a Linux machine gives, and only that: a run under Wine is not a run on Windows. [dm/host/tests/run_wine_tests.sh](../dm/host/tests/run_wine_tests.sh) runs the test programs of a cross-built tree under Wine 9.0 in the `nxnative-wine` image and writes `logs/wine/` (a folder of its own: `logs/win64/` is a real PC's). From the repository root, after the Windows cross-build and, for the AOT tests, the packs of [The recompiled corpus in the host](#the-recompiled-corpus-in-the-host):

```sh
docker build -t nxnative-wine -f docker/Dockerfile.wine docker
sh dm/host/tests/run_wine_tests.sh --packs build/packs/win64 build/win64
```

It runs on the host (it starts the container itself, as your user, with every folder at its own path) and reuses `run_host_tests.sh` inside the image: the same lists (`host_tests.tsv`, `host_scripts.tsv`), logs and environment, with two switches that change nothing when unset: `WINE=/usr/lib/wine/wine64` runs each program through Wine and gives the programs Z:/ paths (Wine's drive for `/`; the script tests, whose Python is then Linux's, get the Unix paths and run `nxhost-run.exe` through `WINE`), and `NOT_RUN=<file>` names the runs not to start. `--packs` (else `NXAOT_PACKS`, else `<build>/packs/win64`) is mounted read-only; `TIMEOUT=<seconds>` per test (default 120). Exit status 0 when nothing failed, 1 when a run failed, 2 on a usage error or a missing tree or image.

`summary.txt` starts with `Wine, not Windows` and the Wine version, then `run_host_tests.sh`'s summary with four kinds of lines besides `PASS`, `FAIL` and `TIMEOUT`: `DIFFERS` (a failure that [dm/host/tests/wine_differences.tsv](../dm/host/tests/wine_differences.tsv) explains as Wine's behaviour, not the port's: every failed check of the log must be one a row names, with that row's evidence in the log, so a new failure in the same test stays a `FAIL`), `SKIP` (a program that passed by skipping its checks: its last line says `skipped`, as the `t_vk_*` programs do when no Vulkan device meets the baseline), and `NOT RUN` (a row of kind `not-run`: `t_second_pack`, whose Python driver needs a Windows Python). A row is added only with its evidence: the Windows API, what Wine does instead, a reproduction or Wine's own `fixme:` line.

What the image and the runner do for Wine, and why:

- **Xvfb.** Wine's Vulkan (`winevulkan.dll`, over the image's lavapipe) needs a graphics driver that supports Vulkan; without a display Wine has none (`Failed to load Wine graphics driver supporting Vulkan`) and every `t_vk_*` program sees no device. The programs themselves draw on SDL's dummy drivers.
- **A tmpfs at `/run/user/<uid>`.** Wine 9.0 keeps its server directory there; without it, it falls back to a path built from `TMPDIR`, and with `TMPDIR` set (the tmpdir tests) it aborts at start in glibc (`free(): invalid pointer`), before any trace.
- **`LANG=C.UTF-8`.** Wine writes a Windows file name to the Unix file system in the locale's character set; in the image's default C locale, creating a file with any non-ASCII name fails (`STATUS_OBJECT_NAME_NOT_FOUND`), which NTFS never does (`t_fs_case`'s non-ASCII saves).
- **`DM_REPO`.** `t_pipe` looks for the checkout above the build folder; a tree built outside the checkout has none there.
- **Crashes.** Wine's crash dialog is off: a crash prints winedbg's backtrace into the test's log and ends the test. The exit code is the low byte of the NTSTATUS (an access violation, `0xC0000005`, is `5`).

No game files are involved; the logs hold test output only ([logs/README.md](../logs/README.md)).

## The shader chain for Vulkan

The Vulkan target of the shader chain (milestone P2 of [vulkan-backend.md](../docs/design/vulkan-backend.md)): the Vulkan translator `dm/gfx/tools/dmsh_xlate_vk` (C#, .NET 10), the SPIR-V passes `dmsh_spv.py`, the build `dmsh_build_vk.py` and its version `dmsh_chain_vk.py` ([dm/gfx/README.md](../dm/gfx/README.md)). The Metal chain does not use this image and nothing here changes it. [Dockerfile.chain](Dockerfile.chain) starts from the .NET SDK 10.0.404 image (runtime 10.0.15; the image of 2026-11-10, the newest 10.0 patch on 2026-11-15) and builds the SPIR-V tools in a stage of its own with [build_spirv_tools.sh](build_spirv_tools.sh), the script that builds the port's bundles' copies (about 4 minutes; its first three instructions are Dockerfile.build's, so a machine that has the build image takes the toolchain from the build cache); the image takes 1.99 GB on disk, most of it the SDK and the 382 MB clang 18 layer that Native AOT needs ([below](#the-ports-tools); clang 18.1.3, `libllvm18` and binutils 2.42 from Ubuntu 24.04, which also upgrades the base image's `libc6` 2.39-0ubuntu8.9 to 8.12). apt resolves the Ubuntu packages when the image is built and the build prints them; on 2026-10-29 they were:

| Tool | Version | Where `dmsh_chain_vk.lock` records it |
|---|---|---|
| .NET SDK | 10.0.404 (runtime 10.0.15) | the translator's assemblies (`dmsh_xlate_vk.dll`, `Ryujinx.Graphics.Shader.dll`, `Ryujinx.Common.dll`), its embedded source hash and the published translators (`dmsh_xlate_vk_linux-x64`, `dmsh_xlate_vk_win-x64`) |
| `clang-18` | 1:18.1.3-1ubuntu1 | the published linux-x64 translator (Native AOT links with it) |
| SPIRV-Tools (`spirv-val`, `spirv-link`, `spirv-opt`, `spirv-dis`, `spirv-as`) | tag `v2025.1` with SPIRV-Headers `09913f088a1197aba4aefd300a876b2ebbaa3391` (its `DEPS`), built by `build_spirv_tools.sh` (`SPIRV-Tools v2025.1 unknown hash, 2025-02-25T20:54:39+00:00`: the tag's commit time) | `spirv-tools_linux-x64`, `spirv-tools_win-x64` |
| glslang (`glslangValidator`) | tag `15.1.0`, with that SPIRV-Tools as its optimizer (`Glslang Version: 11:15.1.0`) | `glslang_linux-x64`, `glslang_win-x64` |
| SPIRV-Cross | tag `vulkan-sdk-1.4.363.0`, commit `f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7` (`Git commit: vulkan-sdk-1.4.363.0 Timestamp: 2026-09-16T11:27:19`) | `spirv-cross_linux-x64`, `spirv-cross_win-x64` |
| `python3`, `python3-yaml` | 3.12.3-0ubuntu2.1, 6.0.1-2build2 | not recorded |

The translator is built with `dotnet build` and run with `dotnet dmsh_xlate_vk.dll` (framework-dependent, as `dmsh_xlate_bin` is); its assemblies are not committed. They are byte for byte the same in any checkout and at any commit as long as they are built into `build/xlate_vk`: the project maps the repository root out of the compilers' paths, and keeps the commit out of the assemblies (`IncludeSourceRevisionInInformationalVersion`: the SDK's Source Link put the checkout's HEAD into every assembly's informational version, so until 2026-11-15 the lock held only at the commit it was written at, 34e0785, and its check failed in every other checkout). Players get the translator published as a program that needs no .NET ([The port's tools](#the-ports-tools)), which `dmsh_build_vk.py` runs as it is when `XLATE_VK` names it; the lock fingerprints both. From the repository root:

```sh
docker build -t nxnative-chain -f docker/Dockerfile.chain docker

# the Vulkan translator, into build/xlate_vk (dmsh_chain_vk.py looks for it there; XLATE_VK names another dll)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-chain \
  dotnet build -c Release -m:1 dm/gfx/tools/dmsh_xlate_vk --artifacts-path build/xlate_vk

# the tools against the lock, then the tests on synthetic Maxwell shaders (no game files)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-chain sh -c \
  'python3 dm/gfx/tools/dmsh_chain_vk.py --check-tools && python3 -m unittest dm/gfx/tests/test_dmsh_vk.py dm/tools/tests/test_nsp_chain.py'

# a game's shaders: its shader package folder and an output folder outside the repository, both on the drive of its NSP folder (AGENTS.md)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e DM_GAME=<base game title ID> -v "$(pwd):/src" -v "<NSP folder>:/nsp" -w /src nxnative-chain \
  python3 dm/gfx/tools/dmsh_build_vk.py /nsp/<shader package folder> /nsp/<id>-work/vk
```

Until 2026-11-15 the image took SPIRV-Tools and glslang from Ubuntu 24.04's packages (`spirv-tools` 2025.1~rc1, whose line was `SPIRV-Tools v2025.1 unknown hash, 2026-05-13T06:03:38+00:00`, and `glslang-tools` 15.1.0) and cloned SPIRV-Cross; the bundles needed the same tools for Windows, which Ubuntu's packages cannot give, so both now come from one script and the same sources, and the lock records the same three lines for `linux-x64` and `win-x64` (the `win-x64` rows are the lines the Windows builds print, written by hand: `--update-lock` measures its own platform only and keeps the other platform's rows). That changed the lock, so `DMSH_CHAIN_VK` (`e58c15d9c4a2` to `997e9319871e`): every packed game builds its Vulkan libraries again.

After a tool change (the SDK, a package, a SPIR-V tool's pin in `build_spirv_tools.sh`, a Ryujinx source, the translator's own files), rebuild the translator, publish it for both platforms ([The port's tools](#the-ports-tools)), run `XLATE_VK_PUBLISHED=build/port_tools python3 dm/gfx/tools/dmsh_chain_vk.py --update-lock` (it stops without the published translators, whose rows the bundles need) and `--header` in the image, and commit `dm/gfx/tools/dmsh_chain_vk.lock` and `dm/gfx/nvn/dmsh_chain_vk.h` with the change. From Git Bash on Windows the commands take the changes of the section above (`MSYS_NO_PATHCONV=1`, `$(pwd -W)`, no `--user`).

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-chain sh -c \
  'dotnet build -c Release -m:1 dm/gfx/tools/dmsh_xlate_vk --artifacts-path build/xlate_vk && sh dm/tools/publish_port_tools.sh linux build/port_tools/linux-x64 &&
   sh dm/tools/publish_port_tools.sh windows build/port_tools/win-x64 && XLATE_VK_PUBLISHED=build/port_tools python3 dm/gfx/tools/dmsh_chain_vk.py --update-lock &&
   python3 dm/gfx/tools/dmsh_chain_vk.py --header'
```

## The port's tools

A player packs a game with the bundle's packer ([docs/development.md, The port's bundles](../docs/development.md#the-ports-bundles)), which needs two C# programs: `nx_extract` (`dm/tools/nx_extract`, the NSP extraction with LibHac) and, for an NVN game, the Vulkan translator above. [dm/tools/publish_port_tools.sh](../dm/tools/publish_port_tools.sh) publishes both as programs that run on a PC with no .NET installed, one platform per run, into `<out>/nx_extract[.exe]`, `<out>/dmsh_xlate_vk[.exe]` and `<out>/licenses/` (the .NET runtime's licence and third-party notices, Ryujinx's, and every NuGet package the two projects restore: LibHac, and Ryujinx.Common's MsgPack.Cli, Humanizer, Gommon, Microsoft.IO.RecyclableMemoryStream and System.CodeDom, which trimming removes from the programs all the same), and the packer run from a bundle takes them from its `bin/` ([scripts/build_ipa_from_nsp.py](../scripts/build_ipa_from_nsp.py) `bundled_tool`). A repository checkout still builds `nx_extract` with `dotnet build` at the first pack and runs the translator's dll with `dotnet`.

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-chain sh dm/tools/publish_port_tools.sh linux build/port_tools/linux-x64 --check
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-chain sh dm/tools/publish_port_tools.sh windows build/port_tools/win-x64 --check
```

`--check` stops (exit 1) when the published translator is not the one the lock fingerprints (`dmsh_chain_vk.py --check-published`). The forms were chosen by measuring each one on 2026-11-15 (this image with SDK 10.0.402; the sizes are SDK 10.0.404's):

| Program | Form | Size | Start (`--source-sha`, usage) | The 40 test shaders | Deterministic |
|---|---|---|---|---|---|
| `nx_extract` linux-x64 | Native AOT | 2.47 MB | 6.8 ms | | yes, also from another folder (no PDB) |
| `dmsh_xlate_vk` linux-x64 | Native AOT | 3.58 MB | 8.6 ms | 23 ms | yes |
| `nx_extract.exe` win-x64 | self-contained single file, trimmed | 13.29 MB | not run here | | yes |
| `dmsh_xlate_vk.exe` win-x64 | self-contained single file, trimmed, ReadyToRun | 22.28 MB | not run here | | yes |

For comparison, on linux-x64: the framework-dependent `dotnet dmsh_xlate_vk.dll` starts in 46 ms and translates the 40 shaders in 313 ms; as a single file 78.4 MB untrimmed (44 ms, 427 ms), 17.4 MB trimmed (87 ms, 609 ms: trimming drops the framework's precompiled code, so everything is compiled by the JIT at each start), 14.2 MB trimmed and compressed (104 ms, 573 ms), 24.2 MB trimmed with ReadyToRun (53 ms, 166 ms); `nx_extract` 15.4 MB as a trimmed single file. Native AOT cannot target Windows from Linux, hence the single files there; ReadyToRun only for the translator, which `dmsh_build_vk.py` starts once per batch of shaders, while `nx_extract` runs a few times per pack.

- **Linux**: `ldd` of both lists only `libc` and `libm` (symbols up to `GLIBC_2.34`: Ubuntu 22.04 runs them too), in a fresh `nxnative-build` container without .NET; `nx_extract` loads OpenSSL 3 (`libssl.so.3`, `libcrypto.so.3`) at run time for its hashes, as every .NET program on Linux does (every distribution's base system has it). Neither needs ICU: `nx_extract` and the translator are built with `InvariantGlobalization`.
- **Windows**: both import only the system's DLLs and the UCRT (`KERNEL32`, `ADVAPI32`, `USER32`, `SHELL32`, `OLEAUT32`, `ole32`, `VERSION`, `api-ms-win-crt-*`, `api-ms-win-core-winrt-l1-1-0`), no Visual C++ runtime. Not run here (no Windows machine): the PC's runner checks them ([below](#running-the-windows-tests-on-the-pc), `-PortTools`).
- **Trimming and AOT**: the translator wrote its reflection through `JsonArray.Add<T>`, which needs reflection-based serialization metadata that a trimmed or AOT program does not have: its AOT build threw `NotSupportedException` on a shader with a 1D texel fetch (`fetch1d`), so `Program.cs` adds every element as a `JsonNode` (the same JSON). The other warning (IL2075, `DynamicObjectFormatter` in Ryujinx.Common's logging) is in code neither the trimmer nor the AOT compiler keeps. LibHac gives none. The trimmed forms were run in their linux-x64 builds: the translator gave the same bytes as the framework-dependent one on all 40 shaders, `nx_extract` passed `dm/tools/tests/test_nx_extract.py`.
- **The same output**: on the 40 synthetic shaders of `dm/gfx/tests/test_dmsh_vk.py` (its 12 graphics and 27 compute shapes, and a 1D texel fetch: 41 SPIR-V modules and 41 reflections), the published linux-x64 translator, run without .NET, wrote the same bytes as the framework-dependent build of the source before these changes (`dotnet build`). `test_dmsh_vk.py` passes with `XLATE_VK` naming the published translator and `DOTNET=/nonexistent`.
- **Resolved** (2026-11-15): the translator is not the NVN step's only tool: `dmsh_build_vk.py` also runs SPIRV-Tools (`spirv-val`, `spirv-link`) and SPIRV-Cross, and checks them and glslang against the lock's rows of the platform it runs on (`dmsh_chain_vk.py --check-tools`). The bundles now carry them, built for both platforms by [build_spirv_tools.sh](build_spirv_tools.sh) and locked for both (below).
- **Resolved** (2026-11-15): SDK 10.0.402's runtime 10.0.13 had a high-severity advisory in its Brotli decoder ([GHSA-p3jp-9v83-76w9](https://github.com/advisories/GHSA-p3jp-9v83-76w9), fixed in runtime 10.0.14), which the Windows single files carried (unused: neither tool decompresses Brotli, and the AOT programs hold no Brotli code). The image is on SDK 10.0.404 with runtime 10.0.15, and `dotnet publish` no longer warns NU1903 on the AOT compiler package. The move changed `Ryujinx.Common.dll` (the one assembly of the three with source-generated code, its regular expressions, whose generator comes with the SDK: the likely cause, not traced) and the four programs, so the lock and the chain version; `dmsh_xlate_vk.dll`, `Ryujinx.Graphics.Shader.dll` and the SPIR-V of the 40 test shaders are the same bytes.

### The packer's Python and the SPIR-V tools

The two other inputs a bundle needs so that a player installs nothing to pack ([docs/development.md, The port's bundles](../docs/development.md#the-ports-bundles)), each made in a container at bundle-build time with every download pinned by URL and SHA-256 in the script (a mismatch stops it with a `[RED] ` line), and each with a `--check` and a `licenses/` folder:

```sh
# the packer's own Python (CPython 3.14.10 with lz4 4.4.5, capstone 5.0.9, fontTools 4.68.0): any image with Python 3; <out>/python/, licenses/, python.tsv
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build python3 dm/tools/publish_port_python.py linux build/port_python/linux-x64 --check
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build python3 dm/tools/publish_port_python.py windows build/port_python/win-x64 --check

# the SPIR-V tools of dmsh_chain_vk.lock: Linux in the build image, Windows cross-built with llvm-mingw; <out>/spirv-val, spirv-link, spirv-cross, glslangValidator, licenses/
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh docker/build_spirv_tools.sh linux build/port_spirv/linux-x64 --check
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src mstorsjo/llvm-mingw:20261020 sh docker/build_spirv_tools.sh windows build/port_spirv/win-x64 --check
```

[dm/tools/publish_port_python.py](../dm/tools/publish_port_python.py) takes CPython 3.14, whose standard library has `compression.zstd` (no `backports.zstd`), since lz4 and capstone have wheels for it on both platforms (PyPI, 2026-11-15: `lz4` 4.4.5 `cp314` manylinux and `win_amd64`; `capstone` 5.0.9 `py3-none` for both; `fontTools`' pure wheel). Linux: [python-build-standalone](https://github.com/astral-sh/python-build-standalone)'s `install_only_stripped` build of release 20261026, a relocatable CPython whose `bin/python3.14` links its extension modules in and needs glibc's own libraries only (from glibc 2.17), without what a packer never runs (tkinter and Tcl/Tk, IDLE, pip and ensurepip, the headers, terminfo, the shared libpython) and without symbolic links; the licences of what is linked into it come from `python/licenses/` of the same release's full archive (pgo+lto, 115 MB, downloaded for them alone). Windows: python.org's embeddable package for amd64, without its `python314._pth`: that file turns on isolated mode with `safe_path`, which keeps a script's own folder off `sys.path`, and the packer's tools import their neighbours that way (`patch_nso.py` imports `nso_asm` before it adds its folder); without it Python takes its prefix from `python314.zip` beside `python314.dll` and its extension modules from its own folder (CPython's `Modules/getpath.py`, the Windows quirks), and `site` adds `Lib\site-packages`, where the wheels are unpacked. The bundle's `pack.cmd` and `pack.sh` clear `PYTHONHOME` and `PYTHONPATH` and set `PYTHONNOUSERSITE` for it. Measured on 2026-11-15:

| Piece | Files | Size | Its check |
|---|---|---|---|
| Python linux-x64 | 1,045 | 67.5 MB | here: the probe (the three modules and `compression.zstd`, a round trip each), every `sys.path` entry inside the bundle, `pack.sh --help`, in a plain `ubuntu:24.04` without Python too |
| Python win-x64 | 472 | 38.0 MB | the files; under Wine 9.0 (`nxnative-wine`, not Windows): the probe, `safe_path` off, `sys.path` inside the bundle, `pack.cmd --help`, `patch_nso.py`'s neighbour import, `make_ext_font.py` with fontTools |
| SPIR-V tools linux-x64 | 4 | 17.5 MB (`spirv-val` 2.8, `spirv-link` 3.6, `glslangValidator` 7.5, `spirv-cross` 3.6) | the lock's lines; `libc` and `libm` only, glibc symbols up to 2.38 |
| SPIR-V tools win-x64 | 4 | 16.3 MB (2.5, 3.3, 6.9, 3.6) | `KERNEL32` and the UCRT only (`llvm-readobj`); under Wine: the lock's lines |

`build_spirv_tools.sh` builds the same bytes in the build image and in the chain image's stage (the same Ubuntu 24.04 clang 18; `-ffile-prefix-map` keeps the work folder out). The Linux and the Windows builds are of the same source and print the same lines, and on the 48 GLSL shaders of `dm/gfx/tests/vk/shaders` they gave the same results under Wine: `glslangValidator` the same SPIR-V bytes, `spirv-link` (each module alone) the same bytes, `spirv-val` the same verdicts, `spirv-cross --msl` the same text but for its line ends (CR LF on Windows, from a C++ stream in text mode; the chain reads its output from a pipe in text mode, which takes them out). That is why the lock's two platforms share one `DMSH_CHAIN_VK` without a guarantee of identical bytes: a Vulkan library is built on the PC that plays it and never moves to the other platform, so the two need the same translation, which the same translator source and the same SPIR-V tools' source give, not the same bytes.

## The recompiled corpus in the host

The host's run time of recompiled guest code (`dm/host/aot_*.c`, [aot.h](../dm/host/aot.h): the arena below 2^39, `dm_guest_call`, the dispatcher, the SVC path, guest threads with safepoints, the fault hooks, the floating-point helpers) is checked by `t_aot_host` on the corpus of the recompiler ([dm/tools/aot/README.md](../dm/tools/aot/README.md#threads-and-safepoints)): self-checking NROs with no game files, packed by `nxaot build` in the recompiler's image, then run in the host built here, each through the host's own loader, SVC dispatcher and thread layer. Without packs the test prints a note and passes, as it does on the Windows runner. From the repository root:

```sh
# the packs: nxaot built in its image (dm/tools/aot/README.md), then every corpus NRO packed into build/packs (18 shared objects, about 2.5 MB), and the
# synthetic game module of t_aot_module into build/packs/module (dm/tools/tests/aot_synth.py: its manifest, behaviour image and pack); then all of them again
# as Windows packs (DLLs, nxaot build --target windows) into build/packs/win64, for the Windows PC (below)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-aot sh -c \
  'cmake -S dm/tools/aot -B build/aot -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/aot --target nxaot && sh dm/tools/aot/tests/build_packs.sh build/aot build/packs'

# the host with the packs, and t_aot_host (-V prints its table: every NRO, the threaded runs, the pause test, the host call, the dirty pages)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh -c \
  'cmake -S dm/host -B build/linux -G Ninja -DGAME=010015100B514000 -DNXAOT_PACKS=/src/build/packs && cmake --build build/linux && ctest --test-dir build/linux -R t_aot_host -V'

# the same tree's game-module tests: t_aot_module (the synthetic module through loader.c's game-module path and dm_aot_run) and t_guest_map (the memory SVCs'
# be_guest_map / be_guest_unmap through a stub backend; it needs no packs)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh -c \
  'cmake --build build/linux --target t_aot_module t_guest_map && ctest --test-dir build/linux -R "t_aot_module|t_guest_map" -V'

# with the JIT fallback in the recompiler's image too (NXHOST_JIT; SDL as a console build there, which has no X11 or Wayland headers)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-aot sh -c \
  'cmake -S dm/host -B build/host-jit -G Ninja -DGAME=010015100B514000 -DNXHOST_JIT=ON -DSDL_UNIX_CONSOLE_BUILD=ON -DNXAOT_PACKS=/src/build/packs && cmake --build build/host-jit --target t_aot_host && ctest --test-dir build/host-jit -R t_aot_host -V'

# the host's floating-point helpers (dm/host/aot_fp.c) against nxaot's (runtime.cpp, over dynarmic), in the recompiler's image
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-aot sh dm/host/tests/aot_fp_diff.sh build/fpdiff
```

**On the Windows PC** the same four AOT tests (`t_aot_host`, `t_aot_module`, `t_aot_hostfn`, `t_host_program`) load the Windows packs: DLLs that `nxaot build --target windows` makes ([dm/tools/aot/README.md, Windows packs](../dm/tools/aot/README.md#windows-packs)), which import from no module and have their run-time entries bound by the loading program (`aot_arena.c`, `nxaot_imports`), so one pack serves `nxhost-run.exe` and every test program. `build_packs.sh` writes them into `<out>/win64`: `<name>.dll` per corpus NRO and `module/`, `hostfn/` with their inputs and `main.aot.dll`, `sub.aot.dll`, and `second_pack/` for the script test `t_second_pack`, which has no `nxaot` on Windows: its game (`main.image.bin`, `main.aot.tsv`), the misses a first session records (`main.misses.tsv`) and the two packs `nxaot` makes of it, `first/main.aot.dll` and `misses/main.aot.dll` (with `--misses`), each with its `nxaot_main.log` (`<out>/second_pack` holds the same for Linux, `.aot.so`). The PowerShell runner ([below](#running-the-windows-tests-on-the-pc)) looks for them in `build\packs\win64` of the checkout (the folder the command above writes, on Linux or from Git Bash with Docker Desktop; copied there from another machine; or CI's: the `aot` workflow uploads that folder as the artifact `win64-packs`, kept 7 days, which unzipped into `build\packs\win64` of a checkout of the same commit serves as is), or takes `-Packs <folder>` or `NXAOT_PACKS`; it sets `NXAOT_PACKS` for every test it runs (`t_second_pack` too) and gives `t_aot_host` the checkout's corpus (`--corpus dm\tools\aot\corpus`: the path compiled in is the container's). Without them the four tests pass with a note, as before. From Git Bash, `run_host_tests.sh` takes `NXAOT_PACKS` from the environment only. The cross-build has the JIT fallback ([The JIT fallback](#the-jit-fallback)), so on the PC as on Linux all 18 corpus programs of `t_aot_host` run to their expected output (fp and fiber through the JIT) and `t_aot_module` makes its JIT-forced run and fault; a tree configured with `-DNXHOST_JIT=OFF` expects 14 of them and 4 stops with status 3, as `summary.txt`'s `engine:` line then says.

`NXAOT_PACKS` in the environment does the same as the CMake option for a tree configured without it. `t_aot_module` takes the module from `<packs>/module` ([dm/tools/aot/README.md, A game module in the host](../dm/tools/aot/README.md#a-game-module-in-the-host)) and passes with a note without it, as `t_aot_host` does; it writes the copies its two refusals load into `t_aot_module.tmp/` in the build folder. In `nxnative-build` the JIT fallback is built in by default ([The JIT fallback](#the-jit-fallback)), so the corpus programs that need it (fp, fiber, except-gcc) run to their expected output through it; in a tree without it (`-DNXHOST_JIT=OFF`) they stop with status 3 and a `[RED] ` line naming the address, as the test then expects. A machine short of disk keeps the build trees in memory (`--tmpfs /src/build:exec,uid=$(id -u),gid=$(id -g)`) and the packs in a folder on disk mounted into the second container.

## Running the host program

`nxhost-run` ([dm/host/main_x86.c](../dm/host/main_x86.c); CMake option `NXHOST_PROGRAM`, on by default) is the host program on Linux and Windows, the counterpart of Apple's `dmhost`: it takes the game's recompiler input as `dmhost` takes the rewritten modules, maps the modules through the loader's game-module path, runs the SDK start (nn::init::Start's steps) and `nnMain` as guest calls on the engine's main thread, opens the SDL3 window with `DM_WINDOW` and shows the game through the Vulkan backend's window presenter, and exits with the run's status (0 when the game ended its process; [aot.h](../dm/host/aot.h) lists the others). The same `DM_*` switches mean the same as on Apple ([docs/env_switches.tsv](../docs/env_switches.tsv)); `DM_AOT_RAW=1` runs a module without the SDK from its own entry (homebrew), `DM_VK_HEADLESS=1` keeps the frames off the window, `DM_VK_LATENCY=low` takes the low-latency swapchain of decision D12 and `DM_VK_PRESENT_MODE=fifo|mailbox` forces the present mode (a diagnostic), `DM_PRESENT_UPSCALE=fsr|integer` scales with AMD FSR 1 or by a whole multiple (decision D4). The JIT fallback is in the program when it is built with `NXHOST_JIT` (by default, on Linux and in the Windows cross-build); the start line says which. It is one program for every game profile ([dm/host/game_rt.c](../dm/host/game_rt.c)): the profile's constants come from a profile file, `--profile <file>` before the pack folder (`dm/games/game.py runtime <id> <file>` writes it; the packer writes `<id>_build/game-profile-<platform>.tsv` for every pack, from a registered, derived or provisional profile, and `run.sh`, `run.cmd` and the launcher pass it), or from the table of every profile of `dm/games` built into it, `--game <title ID>`; with neither, the tree's `-DGAME` (its default). Its first log line names the profile and where it came from (`[dm] game profile <id> (<name>): the file <file> (--profile)`).

```sh
# pack the game on the system it runs on (docs/development.md, "Packing for Windows and Linux"): <id>_build/aot_<version>/ (the max mode's packs in aot_<version>/max/),
# the Vulkan shaders, <id>_build/game-profile-linux.tsv, <id>_build/launch-linux.tsv and <id>_build/run.sh (game-profile-windows.tsv, launch-windows.tsv and run.cmd on
# Windows, whose packs are DLLs): run.sh runs the command below with the pack's own paths, which it reads from the launch file, as the launcher nxlauncher does; nxaot is
# build/aot/nxaot unless --nxaot names it (on Windows the
# bundle's bin\nxaot.exe, or from a checkout --nxaot build\aot-win64\nxaot.exe: dm/tools/aot/README.md, "Building nxaot for Windows")
python3 scripts/build_ipa_from_nsp.py "<NSP folder>" -k <prod.keys> --platform linux

# <id>_build/aot_<version>/ holds, per module, <name>.aot.tsv and <name>.image.bin (patch_nso.py --aot) and <name>.aot.so (nxaot build; .aot.dll on Windows),
# and nro.tsv with a <name>.aot.so per NRO it lists (nxaot build of the NRO file)
DM_WINDOW=1 DM_AUDIO=1 DM_ROMFS=<RomFS image> DM_SAVE=<save folder> DM_TEMP=<temp folder> DM_MSL_DIR=<shader cache> build/linux/nxhost-run \
  --profile "<NSP folder>/<id>_build/game-profile-linux.tsv" "<NSP folder>/<id>_build/aot_<version>" [module...] [-- game arguments]

# the synthetic module of t_aot_module (the packs of "The recompiled corpus in the host" above), with SDL's dummy drivers: what t_host_program runs
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e DM_AOT_RAW=1 -e DM_DBG=1 -e SDL_VIDEO_DRIVER=dummy -e SDL_AUDIO_DRIVER=dummy -v "$(pwd):/src" -w /src nxnative-build \
  build/linux/nxhost-run build/packs/module main
```

No game has run through it yet (no game's modules have been packed): what is open is listed under [What is not portable yet](#what-is-not-portable-yet). CTest checks it with `t_host_program` (the program as a child process on the synthetic module, with and without the window path, the SDK start's refusal of a module set without `nnMain`, the usage) and its presenter with `t_vk_present` (on lavapipe with `VK_EXT_headless_surface`: the swapchain, the picture read back from the swapchain images, the letterbox, the flip, a new window size, the vsync source; `--mailbox`, `--headless`). On a desktop with a screen, the first run of a frame-producing module should check that the window shows the picture upright and letterboxed, that resizing and F11 / Alt+Enter remake the swapchain without a `[RED] ` line, the start line's present mode and vsync source (`[hostvk] window presenter: ...`), and that the frame pacing follows the screen (`DM_NVN_TSV`'s `@presented_ms` where present wait is there).

## The launcher

`nxlauncher` ([dm/launcher](../dm/launcher/main.cpp); CMake option `NXHOST_LAUNCHER`, on by default) is the program a player opens to choose a packed game, its settings and its pack mode and to start it ([docs/settings.md, On Windows and Linux](../docs/settings.md#on-windows-and-linux); [docs/development.md, The launcher](../docs/development.md#the-launcher-nxlauncher)). The same tree builds it with Dear ImGui over SDL3's 2D renderer, beside `nxhost-run`, which it starts with the pack's game profile (`--profile`, the launch file's `game_profile` row; in a bundle `bin/nxhost-run` beside it when the launch file's host is gone); `t_launcher_model` and `t_launcher_window` (the program on SDL's dummy video driver) are among the tests:

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh -c \
  'cmake -S dm/host -B build/linux -G Ninja -DGAME=010015100B514000 && cmake --build build/linux --target all nxhost-run nxlauncher && ctest --test-dir build/linux -R launcher'

# Windows: build/win64/nxlauncher.exe, a GUI program (no console window; --list and --command print to the console they are run from)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src mstorsjo/llvm-mingw:20261020 sh -c \
  'cmake -S dm/host -B build/win64 -G Ninja -DGAME=010015100B514000 -DCMAKE_TOOLCHAIN_FILE=/src/dm/host/cmake/x86_64-w64-mingw32.cmake && cmake --build build/win64 --target all nxhost-run nxlauncher'
```

## The images and the verification on a small disk

[scripts/port_docker.sh](../scripts/port_docker.sh) builds `nxnative-build` and `nxnative-aot` and runs the port's local verification, for a machine whose disk cannot hold both images and their build cache at once. `images [build|aot|all]` builds an image only when it is missing or its Dockerfile changed since the script built it (the image carries the label `org.nxnative.port-docker.dockerfile-sha256`, the Dockerfile's SHA-256, and `org.nxnative.port-docker.built-by`; an image built by hand has neither and is used as it is, `--force` rebuilds it), checks the free space on Docker's data root first (the image's size, about 1.9 GB and 2 GB, plus a 1 GB margin; it stops with exit status 3 and lists what could be removed) and prunes the builder cache after each build. `status` shows which images exist and are current, the free space and what a verification needs. `verify` runs the commands of [The recompiled corpus in the host](#the-recompiled-corpus-in-the-host) and [The launcher](#the-launcher) in sequence, with the build trees on tmpfs: nxaot, the corpus packs and nxaot's CTest in `nxnative-aot`, then the host with `NXAOT_PACKS` and `NXHOST_PROGRAM`, its programs and CTest twice (plain and with `DM_AOT_ARENA_CHECK=1`, `NXAOT` set) in `nxnative-build`; the packs, `nxaot` and each step's log go to a work folder (`--work <dir>`, default `${TMPDIR:-/tmp}/nxnative-port-docker`), and it prints each step's result and CTest's totals and exits with 1 when one failed. `verify --low-disk` holds one image at a time: it removes `nxnative-aot` after its stage when that run built it, then builds `nxnative-build`. The host stage runs the shared-LLVM `nxaot` when `nxnative-build` has `libLLVM.so.18.1` (its `llvm-18`, `lld-18` and `clang-18` depend on `libllvm18`) and builds the static one (`-DNXAOT_LLVM_STATIC=ON`) otherwise. `clean` removes the images the script labelled and its work folder, nothing else.

```sh
scripts/port_docker.sh status
scripts/port_docker.sh verify --low-disk
scripts/port_docker.sh clean
```

## Continuous integration

[.github/workflows/chain.yml](../.github/workflows/chain.yml) builds the chain image, the Vulkan translator, checks the lock and runs `test_dmsh_vk.py` and `test_nsp_chain.py` in it, for pull requests and pushes to `main` and `multiplataforma` that change the chain's files; each run's `logs/chain/` is kept as the artifact `logs-chain`.

[.github/workflows/port.yml](../.github/workflows/port.yml) runs the commands above on GitHub for every pull request and every push to `main` and `multiplataforma`, beside the macOS jobs of `tests.yml`. `linux-host` builds the `nxnative-build` image, configures and builds `build/linux` with the JIT fallback required (`-DNXHOST_JIT=ON`), runs CTest and then `run_host_tests.sh`; `win64-cross` cross-builds `build/win64` in the pinned llvm-mingw image, the JIT fallback required there too, and hands the test `.exe` files and `host_tests.tsv` to `win64-test`, which runs `run_win64_tests.ps1` on a Windows runner; `win64-cross` also publishes the Windows prebuilt tools in the chain image ([The port's tools](#the-ports-tools), `--check`), which `win64-test` runs with `-PortTools`. Each run's `logs/linux/` and `logs/win64/` are kept as the artifacts `logs-linux` and `logs-win64` for seven days: a failure on CI is read there. On a push and on `workflow_dispatch` (not on pull requests) the same workflow also makes the port's bundles: `linux-host` and `win64-cross` build the host (the tree's one `nxhost-run`, for every game profile) and the launcher, `nxaot-bundle` the recompiler in its image, `bundle-linux` publishes the Linux prebuilt tools and runs `test_nx_extract.py` and `test_dmsh_vk.py` with them and without dotnet, and `bundle-linux` and `win64-cross` run `scripts/package_port.py` with `--tools`, uploading `nxnative-linux-x86_64` and `nxnative-windows-x86_64` ([docs/development.md, The port's bundles](../docs/development.md#the-ports-bundles)).

[.github/workflows/aot.yml](../.github/workflows/aot.yml), after the recompiler's own tests, runs the commands of [The recompiled corpus in the host](#the-recompiled-corpus-in-the-host): the packs (the Windows ones uploaded as the artifact `win64-packs`), the `nxnative-build` image and `t_aot_host` in it, `t_aot_host` with the JIT fallback in the recompiler's image, and the floating-point differential test.

## What is not portable yet

- **Running a game.** The x86-64 execution engine's run time is in the host (`dm/host/aot_*.c`: [above](#the-recompiled-corpus-in-the-host)) and runs the recompiler's corpus and a synthetic game-shaped module (`patch_nso.py --aot` writes the recompiler's input, the x86 loader maps it and binds host functions into its GOT: `t_aot_module`), and the host program `nxhost-run` ([below](#running-the-host-program)) runs that module from start to exit, but no real game module has been through it yet: the SDK start and `nnMain` are guest calls that have never met a real SDK. Function addresses cross between the host and recompiled code as on Apple (`t_aot_hostfn`): the C library passthrough is `libc_pass.c`'s PASS entries with host-call thunks (Apple keeps `hle_table.c`), every host function handed to the guest is its guest-visible slot (`dm_guest_fnptr` in `aot.h`: the get-proc-address results through their thunks, Unreal's FMalloc and the ProfanityFilter vtables, the thread entry of `hle_guestthread.c`, whose SDK calls are guest calls), and `hle_ro.c` maps an NRO from the game's image with its pack `<name>.aot.so` (a row in the folder's `nro.tsv`) and runs its init and fini as guest calls. Every place where Apple's host calls guest code through a plain C function pointer is a guest call here (`DM_GUEST_CALL` / `dm_gcall` in `aot.h`; each file keeps Apple's text through its `GUEST_CALL` macro): the NVN layer's command memory callback, the audio renderer's SDK functions (`hle_audren.c`), the movie decoder's Buffer, MediaData and event functions (`hle_movie.c`), the software Opus decoders (`hle_codec.c`), `getenv` and `StandardAllocator::Allocate` (`hle_os.c`), the game's GLSLC (`hle_glslc.c`), the font engine (`hle_sdk16.c`), the system events (`hle_thread.c`), the inline keyboard's callbacks (`hle_swkbd.c`) and `hle_oodle.c`'s decoders, whose tool modes `DM_OODLE_SERVE` and `DM_OODLE_JOB` now run in `nxhost-run`. A guest call nests inside the host call that makes it, on the same guest thread, and leaving guest code (a stop, ExitThread, a fault) unwinds the host frames level by level; a host thread with no guest thread gets a guest context of its own with a `[RED] ` line (no SDK thread there). `t_aot_hostfn` checks nesting, two levels, ten arguments, a host-created guest thread, a plain host thread, the system-event path and a process stop inside a nested call. The host memory the guest sees lies in the arena below 2^39 under one rule (`aot.h`, "Host memory the guest sees": the garena, `dm_garena_*` and the per-thread scoped region `dm_gscope_*`, with each file's `GUEST_*` macros keeping Apple's text): the take-overs' buffers and host-stack arrays (`hle_audren.c`, `hle_codec.c`, `hle_movie.c`, `hle_swkbd.c`, `hle_oodle.c`), the strings and statics they hand out (`getenv`'s answers, device and error names, the event objects of `hle_oe.c`, `hle_friends.c` and `hle_sdk16.c`, `hle_vi.c`'s display objects, `localtime`'s result and `tm_zone`, and the zone names the C library passthrough's `gmtime_r`, `localtime_r`, `mktime` and `timegm` write: `libc_pass.c`), the shared fonts, Unreal's heap (`hle_uemalloc.c` on the garena), NVN's virtual pool ranges and lent command memory, the guest Vulkan layer's mapped memory and the guest OpenGL layer's mapped buffers (arena pages imported with `VK_EXT_external_memory_host`; lavapipe offers no `VK_EXT_map_memory_placed`), and the OpenGL layer's strings (`glGetString`, `glGetStringi`, `eglQueryString`) and fence objects (`guest_gl.c`). `DM_AOT_ARENA_CHECK=1` checks every host-call thunk's pointers and every `DM_GUEST_CALL`'s pointer arguments against it (`t_garena`, `t_aot_hostfn`). Still outside the arena, declared: the opaque host handles stored in guest objects or handed to the guest (the movie decoder's state, `hle_ro.c`'s module record, the guest Vulkan layer's and the driver's handles, the OpenGL layer's EGL display, configuration, context and surfaces), which no guest code reads through, and the OpenGL layer's mappings on a device whose host-pointer imports are refused or not coherent (driver memory, with a `[RED] ` line; none here). The code tied to running the guest's ARM64 code natively stays Apple arm64 only, behind `DM_NATIVE_EXEC` in [plat.h](../dm/host/plat/plat.h): `entry.S`, `dm/gfx/nvn/nvn_entry.S` and `nvn_thunks.S`, `hle_bigstack.c` (the big host stack switch; `HLE_FORCE_BIG` registers the implementation directly elsewhere), the `hle.h` inline stub, the `tpidrro_el0` thread slots in `loader.c`, the Mach thread state and signal-context reads in `hle_thread.c`, `probe.c`, `crash.c` (its fault hooks and the fault report are `aot_fault.c`'s on x86-64), the Mach-O section lookups of embedded modules, and `DM_HLECOUNT`'s counting stubs (a set `DM_HLECOUNT` logs a `[RED] ` line there).
- **Graphics and video output.** NVN, the guest's Vulkan and OpenGL run on the Vulkan backend here (`dm/gfx/hostvk`), on Metal on Apple. The Vulkan backend's window presenter (`vk_present.c`) draws the game into the SDL3 window through a swapchain and paces it from its vsync source, checked on lavapipe with a headless surface only (`t_vk_present`): it has never shown a frame on a screen, with a real GPU, a compositor or a monitor's refresh. The progress screen is drawn with SDL's software renderer until the presenter makes its surface at the first frame, then shows in the window title (always the title on Wayland, which gives SDL no software surface); the text box shows its text in the window title. The spatial upscaler of decision D4 (`DM_PRESENT_UPSCALE=fsr`, AMD FSR 1) and integer scaling are there, checked on lavapipe only; the Linux compositor's presentation feedback and DXGI's `WaitForVBlank` as vsync sources are not written (the presenter's clock runs free without present wait or display timing). H.264 movies show no picture (`hle_movie_avc.c`, VideoToolbox on Apple) and AAC movie sound is silent (`hle_movie.c`, AudioToolbox on Apple), each with a `[RED] ` line. Audio output is SDL3's (`DM_AUDIO=0` chooses the silent sink); VP8 / VP9 (libvpx) and Vorbis (stb_vorbis) decode everywhere.
- **Host unit tests outside the CMake build** (they stay in `dm/host/tests/Makefile`): `t_altstack` (`crash.c`'s signal stack, native execution), `t_audio` and `t_audio_device` (the Core Audio render callback, device and AudioUnit watchdog; `t_audio_sdl` is their SDL3 counterpart here), `t_motion_apple`, `t_motion_pro_mac` and `t_haptics_apple` (Apple front ends: GameController, IOKit, Core Haptics; `t_front_sdl` checks the SDL3 front end here), `t_svc_memory_dirty` (the Metal backend's write-protect page tracking, `dm/gfx/nvn`), and the sanitizer builds (`t_fs_commit_list_tsan`, `t_audio_spsc_tsan`, `t_hid_tsan`, `t_svc_memory_tsan`, `make asan`). The CMake build also compiles `t_heap_size`, `t_svc_memory` and `t_svc_memory_16k` without the undefined-behaviour and address sanitizers the Makefile adds (the image has no sanitizer runtime).
- **Windows only:** the ten death tests that run a check in a `fork()`ed child (`t_heap_size`, `t_oe`, `t_fpslock`, `t_audren_layout`, `t_shared_font`, `t_system_event`, `t_symbols`, `t_svc_memory`, `t_svc_memory_unlimited`, `t_svc_memory_16k`) build and run on Linux only; `host_tests.tsv` and `summary.txt` list them as not built. The descriptor census `DM_FD_LOG` is not available (a set `DM_FD_LOG` logs a `[RED] ` line), `plat_addr_symbol` finds no symbol names (the `DM_IMPORTS` log shows `?`), the save transaction's directory swap is three renames instead of one atomic exchange (`plat_rename_swap` in `plat_win32.c`), and the time zone name the games get (`tzname[0]`, `hle_time.c`, `hle_sdk16.c`) is the UCRT's Windows zone name, which can be localized (llvm-mingw marks `tzname` deprecated there), where Apple gives an abbreviation such as `CET`.
