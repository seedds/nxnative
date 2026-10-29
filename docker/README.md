# The host on Linux and Windows: build and test in Docker

This folder holds the build environment of the portable host for Linux and Windows (x86-64), Phases 0 and 1 of the port in [migration-status.md](../docs/design/migration-status.md). What builds today is the host's platform layer (`dm/host/plat/`), the host sources that do not depend on running the guest's ARM64 code natively, the SDL3 front end and audio output (below), libvpx, SDL3 itself, and the host unit tests that need neither native execution nor an Apple front end. No game runs on Linux or Windows yet. The Apple builds do not use any of this: they keep `make -C dm/host` and `make -C dm/host/tests`.

The build is [dm/host/CMakeLists.txt](../dm/host/CMakeLists.txt). It needs a game profile (`-DGAME=<base game title ID>`, as the host Makefile does; the commands below use Mario's, `010015100B514000`). Build trees go to `build/` at the repository root, which git ignores. Each run of the tests writes its logs to `logs/<platform>/` ([logs/README.md](../logs/README.md)).

## Images

| Use | Image | Contents |
|---|---|---|
| Linux build and tests | `nxnative-build`, built from [Dockerfile.build](Dockerfile.build) | Ubuntu 24.04 pinned by digest; clang and lld 18.1.3, CMake 3.28, Ninja, Python 3.12, git, and the development headers SDL3's Linux drivers compile against (X11, Wayland, libdecor, xkbcommon, EGL, ALSA, PulseAudio, PipeWire, D-Bus, udev, IBus), and Vulkan without a GPU ([below](#vulkan)): Mesa's lavapipe, the Vulkan loader, the validation layer, glslang and SPIRV-Tools |
| Windows cross-build | `mstorsjo/llvm-mingw:20261020` (digest `sha256:dadd757c364edee190c53d8e71a135892a6986182e873205d3611f85e26f290b`) | llvm-mingw: clang 23.1.4 for `x86_64-w64-mingw32` with the UCRT and winpthreads, CMake 3.28.3, Ninja 1.11.1, Python 3.12.3, git |

The Linux image takes about 1.6 GB on disk (the SDL3 headers are a layer of their own, about 340 MB, and the Vulkan packages another, about 190 MB); the llvm-mingw image about 1.6 GB (350 MB to download: it carries toolchains for four architectures). The Windows programs it makes are linked statically (`-static`, SDL3 and libc++ included): they need only the system's own DLLs (`KERNEL32.dll`, `bcrypt.dll` and the UCRT, which every Windows 10 has; the programs with SDL3 also `USER32`, `GDI32`, `IMM32`, `ole32`, `OLEAUT32`, `SETUPAPI`, `SHELL32`, `VERSION`, `WINMM` and `ADVAPI32`), so the test `.exe` files run on a stock Windows 10 or 11. To check a program: `llvm-readobj --coff-imports build/win64/t_front_sdl.exe | grep -i '\.dll'` in the llvm-mingw image.

## SDL3

The front end on Linux and Windows (`dm/host/front_sdl.c`: window, keyboard, mouse as touch, controllers with gyro and rumble, fullscreen with refresh-rate matching, the progress screen, the text box, the save-failure notice) and the audio output there (`dm/host/hle_audio.c`) use SDL3, which [dm/host/CMakeLists.txt](../dm/host/CMakeLists.txt) downloads and builds as a static library at configure time (CMake `FetchContent`):

| Version | Tag | Commit | Released |
|---|---|---|---|
| SDL 3.4.14 | `release-3.4.14` | `147a8ee32dbf9ac02f3794964490687b6bbda1bc` | 2026-08-03 |

It is the newest SDL3 release that was at least two months old when it was chosen (2026-10-28; 3.4.16 and 3.4.18 came out in September and October): a stable bugfix release with time for regressions to be reported. The configure step makes a shallow clone of the tag and stops when its commit is not the one above (a moved tag). Only what the host uses is built: video (X11, Wayland, Windows, the dummy driver), the software renderer (the progress screen), Vulkan surfaces, events, joystick and gamepad (HIDAPI, which drives the PlayStation, Switch and Xbox controllers' gyro and rumble, and the virtual joysticks the tests use), haptic, sensor and audio (PipeWire, PulseAudio, ALSA, WASAPI, DirectSound, dummy). On Linux SDL loads X11, Wayland and the sound servers with `dlopen` at run time, so a program built here starts on a PC without one of them (that driver is skipped); the Linux test programs link only `libc` and `libm`.

Offline, or to build from a copy you already have, point CMake at an unpacked copy of that release: `-DFETCHCONTENT_SOURCE_DIR_SDL3=/src/<path to the copy>` (a path inside the container). Nothing is downloaded then; the commit is checked when the copy is a clone, and the configure step says so when it is not (it must be 3.4.14).

The SDL tests run on SDL's dummy drivers, which they choose themselves (`SDL_VIDEO_DRIVER` and `SDL_AUDIO_DRIVER` set to `dummy` through SDL's hints before SDL starts), so they need no display, sound card or controller, on Linux in Docker as on a Windows runner: `t_front_sdl` (keys, mouse, focus, virtual gamepads as players with buttons, sticks, triggers, motion and rumble, the progress screen, the save-failure notice, the text box) and `t_audio_sdl` (an output through `OpenAudioOut` on the dummy device: callbacks, released buffers, Stop, suspend, Close, no thread left behind).

## Vulkan

The Vulkan backend's tests (`t_vk_device`, `t_vk_import`: [dm/gfx/hostvk](../dm/gfx/hostvk/README.md)) run headless on Mesa's software Vulkan driver, lavapipe, under the Khronos validation layer, installed by a layer of the Linux image of its own. The image is pinned by the digest of its base, but apt resolves the packages from Ubuntu 24.04's archive when the image is built; the build prints the versions (`dpkg-query` at the end of that layer). On 2026-10-29 they were:

| Package | Version | What |
|---|---|---|
| `mesa-vulkan-drivers` | 25.2.8-0ubuntu0.24.04.4 | lavapipe (`libvulkan_lvp.so`, manifest `/usr/share/vulkan/icd.d/lvp_icd.json`, Vulkan 1.4.318), and Mesa's hardware drivers, which find no GPU in the container |
| `libllvm20` | 1:20.1.2-0ubuntu1~24.04.3 | lavapipe's code generator (already in the image: Mesa's EGL in the SDL3 layer needs it) |
| `libvulkan1` | 1.3.275.0-1build1 | the Vulkan loader, which volk loads at run time (already in the image; no `libvulkan-dev`: nothing links against it) |
| `vulkan-validationlayers` | 1.3.275.0-1 | `VK_LAYER_KHRONOS_validation` |
| `glslang-tools` | 15.1.0-2~ubuntu0.24.04.2 | `glslangValidator`, for `dm/gfx/hostvk/tests/gen_spirv.sh` |
| `spirv-tools` | 2025.1~rc1-1~ubuntu0.24.04.2 | `spirv-val`, the same script's check |

Two variables choose them: `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` (with `VK_LOADER_DRIVERS_SELECT=lvp*`, which this loader also reads) makes lavapipe the only driver, and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` turns the validation layer on. CTest sets them for the two tests ([dm/host/CMakeLists.txt](../dm/host/CMakeLists.txt), the `ENV` option of `host_test`), and then a missing driver or layer fails them. The runners do not set them; the tests still run on lavapipe there (the only driver with a device in the container), with the layer they turn on themselves. Ubuntu 24.04's loader is 1.3.275, so the device, which reports Vulkan 1.4.318, is used at Vulkan 1.3. The manifest is `lvp_icd.json` in this Mesa, not the `lvp_icd.x86_64.json` of older packages. The Windows test programs import no Vulkan DLL (volk loads `vulkan-1.dll` at run time): on a PC or runner without a Vulkan loader or driver they print `note: no Vulkan device: skipped` and pass.

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

The toolchain file is given by its absolute path inside the container: CMake looks for a relative `CMAKE_TOOLCHAIN_FILE` in the build folder first and then in `dm/host`, not in the current folder.

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

Both run every test program of `build/win64` with a 120 s limit each (`-TimeoutSeconds` in PowerShell, `TIMEOUT=<seconds>` for the shell script), print `PASS`, `FAIL` or `TIMEOUT` per run and a total, write `logs/win64/<test>.log` and `logs/win64/summary.txt`, and exit with 0 when every run passed and 1 otherwise. `-ExecutionPolicy Bypass` applies to that one command only (Windows refuses unsigned scripts by default); `-BuildDir` and `-LogDir` choose other folders. A test that crashes ends with an NTSTATUS exit code, which the PowerShell runner also prints in hex: `0xC0000005` is an access violation, `0xC0000409` a fast fail, which is also how `abort()` and a failed `assert` end a program built on the UCRT. The PowerShell runner sets `SEM_NOGPFAULTERRORBOX` for the tests it starts (`SetErrorMode`, which child processes inherit), so an access violation ends that test at once instead of waiting on an error dialog; a dialog that still appears (from Git Bash, or for a fast fail where Windows Error Reporting is set to show one) holds the test until its time limit.

### When a run fails: commit the logs

The logs are meant to be read by whoever fixes the failure, here or on another machine, so a failing run's logs are committed and pushed:

```sh
git add logs/win64
git commit -m "Host test logs: Windows, <n> failed"
git push
```

Each log starts with the date, the commit the run was built from (`git rev-parse HEAD`, marked when the working tree had uncommitted changes) and the exact command; `summary.txt` adds the Windows version, the runner and the toolchain that built the tree. [logs/README.md](../logs/README.md) has the convention. The Linux runs in Docker write `logs/linux/` the same way.

## Continuous integration

[.github/workflows/port.yml](../.github/workflows/port.yml) runs the commands above on GitHub for every pull request and every push to `main` and `multiplataforma`, beside the macOS jobs of `tests.yml`. `linux-host` builds the `nxnative-build` image, configures and builds `build/linux`, runs CTest and then `run_host_tests.sh`; `win64-cross` cross-builds `build/win64` in the pinned llvm-mingw image and hands the test `.exe` files and `host_tests.tsv` to `win64-test`, which runs `run_win64_tests.ps1` on a Windows runner. Each run's `logs/linux/` and `logs/win64/` are kept as the artifacts `logs-linux` and `logs-win64` for seven days: a failure on CI is read there.

## What is not portable yet

- **Running a game.** The x86-64 execution engine (the ahead-of-time recompiler, Phase 2) does not exist yet, so neither does a Linux or Windows host program: `main.c` and the front ends are not in the CMake build. The code tied to running the guest's ARM64 code natively stays Apple arm64 only, behind `DM_NATIVE_EXEC` in [plat.h](../dm/host/plat/plat.h): `entry.S`, `dm/gfx/nvn/nvn_entry.S` and `nvn_thunks.S`, `hle_bigstack.c` (the big host stack switch; `HLE_FORCE_BIG` registers the implementation directly elsewhere), the `hle.h` inline stub, the `tpidrro_el0` thread slots in `loader.c`, the Mach thread state and signal-context reads in `hle_thread.c`, `probe.c`, `crash.c`, the Mach-O section lookups of embedded modules, and `DM_HLECOUNT`'s counting stubs (a set `DM_HLECOUNT` logs a `[RED] ` line there).
- **Graphics and video output.** NVN, Vulkan and OpenGL are on Metal (`dm/gfx`, Phase 4 brings Vulkan). The SDL3 front end (above) makes the window the Vulkan presenter will draw into; until the presenter exists nothing draws the game there, the progress screen is drawn with SDL's software renderer (in the window title only on Wayland, which gives SDL no software surface, and once the presenter has made its surface) and the text box shows its text in the window title. H.264 movies show no picture (`hle_movie_avc.c`, VideoToolbox on Apple) and AAC movie sound is silent (`hle_movie.c`, AudioToolbox on Apple), each with a `[RED] ` line. Audio output is SDL3's (`DM_AUDIO=0` chooses the silent sink); VP8 / VP9 (libvpx) and Vorbis (stb_vorbis) decode everywhere.
- **Host unit tests outside the CMake build** (they stay in `dm/host/tests/Makefile`): `t_altstack` (`crash.c`'s signal stack, native execution), `t_audio` and `t_audio_device` (the Core Audio render callback, device and AudioUnit watchdog; `t_audio_sdl` is their SDL3 counterpart here), `t_motion_apple`, `t_motion_pro_mac` and `t_haptics_apple` (Apple front ends: GameController, IOKit, Core Haptics; `t_front_sdl` checks the SDL3 front end here), `t_svc_memory_dirty` (the Metal backend's write-protect page tracking, `dm/gfx/nvn`), and the sanitizer builds (`t_fs_commit_list_tsan`, `t_audio_spsc_tsan`, `t_hid_tsan`, `t_svc_memory_tsan`, `make asan`). The CMake build also compiles `t_heap_size`, `t_svc_memory` and `t_svc_memory_16k` without the undefined-behaviour and address sanitizers the Makefile adds (the image has no sanitizer runtime).
- **Windows only:** the ten death tests that run a check in a `fork()`ed child (`t_heap_size`, `t_oe`, `t_fpslock`, `t_audren_layout`, `t_shared_font`, `t_system_event`, `t_symbols`, `t_svc_memory`, `t_svc_memory_unlimited`, `t_svc_memory_16k`) build and run on Linux only; `host_tests.tsv` and `summary.txt` list them as not built. The descriptor census `DM_FD_LOG` is not available (a set `DM_FD_LOG` logs a `[RED] ` line), `plat_addr_symbol` finds no symbol names (the `DM_IMPORTS` log shows `?`), the save transaction's directory swap is three renames instead of one atomic exchange (`plat_rename_swap` in `plat_win32.c`), and the time zone name the games get (`tzname[0]`, `hle_time.c`, `hle_sdk16.c`) is the UCRT's Windows zone name, which can be localized (llvm-mingw marks `tzname` deprecated there), where Apple gives an abbreviation such as `CET`.
