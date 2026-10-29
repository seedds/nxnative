# The portable host for Linux and Windows (x86-64): the host's portable sources as a static library, libvpx, and the host unit tests that run without native ARM64
# execution or an Apple frontend, registered with CTest. Apple builds keep using the Makefiles (make -C dm/host, make -C dm/host/tests). docker/README.md has the commands.
#   cmake -S dm/host -B build/linux -G Ninja -DGAME=<base game title ID>
# Windows: the same with -DCMAKE_TOOLCHAIN_FILE=<absolute path of dm/host/cmake/x86_64-w64-mingw32.cmake> (llvm-mingw; a relative path is taken from the build folder first, then from
# dm/host). The test .exe files then run on a Windows PC: dm/host/tests/run_win64_tests.ps1 (PowerShell) or dm/host/tests/run_host_tests.sh (Git Bash) read the list this file writes
# (<build folder>/host_tests.tsv) and record a log per test in logs/<platform>/ (logs/README.md). CTest runs the same tests where the tree was configured (Linux, Docker)
cmake_minimum_required(VERSION 3.20)
project(nxnative_host C)

set(GAME "" CACHE STRING "game profile (dm/games/<base game title ID>): the host is built for it")
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
cmake_path(NORMAL_PATH REPO)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT GAME)   # there is no default game: name the profiles, as the Makefile does
    execute_process(COMMAND ${Python3_EXECUTABLE} ${REPO}/dm/games/game.py list OUTPUT_VARIABLE profiles)
    message(FATAL_ERROR "GAME is not set: cmake -DGAME=<base game title ID>. Profiles:\n${profiles}")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "the CMake host build targets x86-64 Linux and Windows (got ${CMAKE_SYSTEM_PROCESSOR}); Apple arm64 builds use dm/host/Makefile")
endif()
if(APPLE)
    message(FATAL_ERROR "Apple builds use dm/host/Makefile and dm/host/tests/Makefile")
endif()

# ---- The game profile header (dm/games/game.py header, as the Makefile writes gen/<GAME>/game_profile.h) ----
set(GEN ${CMAKE_BINARY_DIR}/gen/${GAME})
set(GAME_H ${GEN}/game_profile.h)
set(PROFILE_DIR ${REPO}/dm/games/${GAME})
add_custom_command(OUTPUT ${GAME_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
    COMMAND ${Python3_EXECUTABLE} ${REPO}/dm/games/game.py header ${GAME} ${GAME_H}
    DEPENDS ${REPO}/dm/games/game.py ${PROFILE_DIR}/game.tsv
    COMMENT "game profile header for ${GAME}" VERBATIM)
add_custom_target(game_profile DEPENDS ${GAME_H})

# ---- Compiler settings ----
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)   # GNU C, as on Apple: ?:, typeof, statement expressions
set(HOST_FLAGS -O1 -g -Wall -Wno-unused-function)
if(WIN32)
    # Windows 10 1803+ (VirtualAlloc2 / high-resolution waitable timers era); 64-bit file offsets for game files over 2 GB
    # _POSIX_THREAD_SAFE_FUNCTIONS: mingw-w64's localtime_r / gmtime_r
    set(PLAT_DEFS _WIN32_WINNT=0x0A00 WINVER=0x0A00 _FILE_OFFSET_BITS=64 __USE_MINGW_ANSI_STDIO=0 _POSIX_THREAD_SAFE_FUNCTIONS=200112L)
    set(PLAT_SRC plat/plat_win32.c plat/sha256.c)
    set(PLAT_LIBS pthread bcrypt)   # pthread: mingw-w64's winpthreads
    add_link_options(-static)   # winpthreads and the C++-free runtime linked in: the test .exe files run on a stock Windows
    # The application manifest, in every executable (host_test below; a host executable target takes it the same way): the process ANSI code page is UTF-8, so the C runtime's narrow
    # file calls and getenv agree with the platform layer's UTF-8 paths and a non-ASCII guest name (a Japanese save file) is one name on disk (plat/win32_utf8.manifest, plat_win32.c).
    # A resource in a static library is never linked in (nothing references it), so it is a source of each executable
    enable_language(RC)
    set(WIN32_MANIFEST_RC ${CMAKE_CURRENT_SOURCE_DIR}/plat/win32_utf8.rc)
    set_source_files_properties(${WIN32_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plat/win32_utf8.manifest)
else()
    set(PLAT_DEFS _GNU_SOURCE)
    set(PLAT_SRC plat/plat_linux.c plat/sha256.c)
    set(PLAT_LIBS pthread dl m)
    # The host prints uint64_t with %llx / %llu (Apple's uint64_t is unsigned long long); glibc's is unsigned long, the same width, which -Wformat reports at every log line
    list(APPEND HOST_FLAGS -Wno-format)
endif()

# ---- The platform layer ----
add_library(nxplat STATIC ${PLAT_SRC})
target_compile_options(nxplat PRIVATE ${HOST_FLAGS})
target_compile_definitions(nxplat PUBLIC ${PLAT_DEFS})
target_include_directories(nxplat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nxplat PUBLIC ${PLAT_LIBS})

# ---- libvpx: the VP8 / VP9 decoders, C only (config/generic: no NEON, no x86 assembly) ----
set(VPX ${REPO}/dm/third_party/libvpx)
file(GLOB_RECURSE VPX_SRC CONFIGURE_DEPENDS ${VPX}/*.c)
list(FILTER VPX_SRC EXCLUDE REGEX "/arm/|aarch64_cpudetect")
add_library(vpx STATIC ${VPX_SRC})
target_include_directories(vpx PUBLIC ${VPX} ${VPX}/config/generic)
target_compile_options(vpx PRIVATE -O2 -w)
if(NOT WIN32)
    target_link_libraries(vpx PUBLIC pthread m)
endif()

# ---- SDL3: the Linux and Windows front end (front_sdl.c: window, keyboard, mouse, controllers, motion, rumble) and audio output (hle_audio.c) ----
# Pinned by release tag and the commit it names (docker/README.md has the version, its date and why). A shallow clone of the tag; the commit is checked after it, so a moved tag
# stops the configure step. Offline: -DFETCHCONTENT_SOURCE_DIR_SDL3=<a copy of that release> builds from the copy instead (not downloaded; its commit is checked when it is a git checkout).
# A static library with only what the host uses: video (with the dummy driver the tests run on, X11 and Wayland, Windows), the software renderer for the progress screen, Vulkan
# surfaces, events, joystick and gamepad (HIDAPI for the PlayStation, Switch and Xbox controllers' gyro and rumble), haptic, sensor (the gyro of PC handhelds whose controller has
# none of its own) and audio (PipeWire, PulseAudio, ALSA, WASAPI, the dummy driver). The Linux libraries are loaded at run time with dlopen (SDL_DEPS_SHARED), so a PC without
# one of them still starts: that driver is skipped
set(SDL3_TAG release-3.4.14)
set(SDL3_COMMIT 147a8ee32dbf9ac02f3794964490687b6bbda1bc)
if(WIN32)
    enable_language(CXX)   # SDL's Windows core has C++ sources (GameInput), so the programs that link it link as C++; the language is enabled here, above every target that links SDL
endif()
include(FetchContent)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG ${SDL3_TAG} GIT_SHALLOW TRUE)
foreach(opt SDL_SHARED SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL SDL_UNINSTALL SDL_GPU SDL_CAMERA SDL_POWER SDL_DIALOG SDL_TRAY
            SDL_OPENGL SDL_OPENGLES SDL_RENDER_VULKAN SDL_RENDER_D3D SDL_RENDER_D3D11 SDL_RENDER_D3D12 SDL_KMSDRM SDL_OFFSCREEN SDL_DISKAUDIO
            SDL_JACK SDL_SNDIO SDL_OSS SDL_FRIBIDI SDL_LIBTHAI SDL_LIBURING SDL_HIDAPI_LIBUSB)
    set(${opt} OFF)
endforeach()
foreach(opt SDL_STATIC SDL_VIDEO SDL_RENDER SDL_VULKAN SDL_AUDIO SDL_JOYSTICK SDL_HAPTIC SDL_HIDAPI SDL_SENSOR SDL_DUMMYVIDEO SDL_DUMMYAUDIO SDL_VIRTUAL_JOYSTICK)
    set(${opt} ON)
endforeach()
FetchContent_MakeAvailable(SDL3)
if(NOT FETCHCONTENT_SOURCE_DIR_SDL3 OR EXISTS ${sdl3_SOURCE_DIR}/.git)
    find_package(Git REQUIRED)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${sdl3_SOURCE_DIR} rev-parse HEAD OUTPUT_VARIABLE sdl3_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT sdl3_head STREQUAL SDL3_COMMIT)
        message(FATAL_ERROR "SDL3 at ${sdl3_SOURCE_DIR} is commit '${sdl3_head}', not ${SDL3_COMMIT} (${SDL3_TAG}): the tag moved or the copy is another version")
    endif()
else()
    message(STATUS "SDL3 from ${FETCHCONTENT_SOURCE_DIR_SDL3}, not a git checkout: its version is not checked (it must be ${SDL3_TAG})")
endif()
if(NOT CMAKE_BUILD_TYPE)
    target_compile_options(SDL3-static PRIVATE -O2)   # SDL as a subproject gets no optimization of its own: its audio conversion and mixing run on every device callback
endif()

# ---- The portable host sources ----
# Not here: the parts tied to native ARM64 execution (entry.S, hle_bigstack.c, crash.c, probe.c; the x86-64 execution engine, Phase 2, replaces them), the program entry
# main.c (it runs a frontend), and the Apple frontend files (*.m). front_sdl.c is the front end here. hle_fdstat.c compiles to nothing on Windows
file(GLOB HLE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hle_*.c)
list(FILTER HLE_SRC EXCLUDE REGEX "/hle_bigstack\\.c$")
set(NXHOST_SRC hle.c loader.c thread.c front_sdl.c)
list(TRANSFORM NXHOST_SRC PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(APPEND NXHOST_SRC ${HLE_SRC})
# The host-call thunks of the x86-64 execution engine (docs/design/aot-recompiler.md 2.4): generated from the HLE entries of exactly the sources nxhost compiles,
# so an entry in a file that is not built here gets no thunk. The generator also reads the quoted headers beside them (typedefs, wrapper macros). aot_hostfn.c is the
# host-function page that gives each thunk its guest-visible address
set(GEN_HLE_THUNKS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_thunks.py)
set(HLE_THUNKS_C ${CMAKE_BINARY_DIR}/gen/hle_thunks.c)
file(GLOB HOST_H CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h ${CMAKE_CURRENT_SOURCE_DIR}/plat/*.h)
add_custom_command(OUTPUT ${HLE_THUNKS_C}
    COMMAND ${Python3_EXECUTABLE} ${GEN_HLE_THUNKS} -o ${HLE_THUNKS_C} ${NXHOST_SRC}
    DEPENDS ${GEN_HLE_THUNKS} ${NXHOST_SRC} ${HOST_H}
    COMMENT "host-call thunks from the HLE entries" VERBATIM)
add_library(nxhost STATIC ${NXHOST_SRC} aot_hostfn.c ${HLE_THUNKS_C})
add_dependencies(nxhost game_profile)
target_compile_options(nxhost PRIVATE ${HOST_FLAGS})
target_include_directories(nxhost PUBLIC ${GEN} ${PROFILE_DIR} ${REPO}/dm/third_party/stb_vorbis ${REPO}/dm/tools/aot/include)
target_link_libraries(nxhost PUBLIC nxplat vpx SDL3::SDL3-static)

# ---- Unit tests (dm/host/tests: each includes the host sources it exercises, so a test links the platform layer, not nxhost) ----
# host_test(<name> SOURCES <files> [PROFILE] [VPX] [SDL] [POSIX] [DEFS <macros>] [RUNS <arguments; one run each, "-" = none>] [TMPDIR])
#   PROFILE: needs the game profile header; SDL: links SDL3 (front_sdl.c, hle_audio.c); POSIX: needs fork() (death tests in a child), so Linux only for now
include(CTest)
set(TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
# The test list for the runners (dm/host/tests/run_host_tests.sh, run_win64_tests.ps1), which run where CTest cannot: CTest's files name this machine's paths, and a Windows tree
# cross-built in Docker runs on the PC. One line per run, tab-separated: name, executable, arguments (separated by spaces), "tmpdir" when it needs TMPDIR=<build folder>/tmp;
# an empty field is "-" (a POSIX shell's read merges adjacent tabs)
if(WIN32)
    set(LOG_PLATFORM win64)
else()
    set(LOG_PLATFORM linux)
endif()
string(TIMESTAMP configured "%Y-%m-%dT%H:%M:%SZ" UTC)
set(MANIFEST "# nxnative host unit tests, written by dm/host/CMakeLists.txt: name<TAB>executable<TAB>arguments<TAB>tmpdir\n")
string(APPEND MANIFEST "# platform: ${LOG_PLATFORM}\n# system: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}\n# compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER})\n")
string(APPEND MANIFEST "# game: ${GAME}\n# configured: ${configured}\n")
function(host_test name)
    cmake_parse_arguments(T "PROFILE;VPX;SDL;POSIX;TMPDIR" "" "SOURCES;DEFS;RUNS;INCLUDES" ${ARGN})
    if(T_POSIX AND WIN32)
        string(APPEND MANIFEST "# not built for this platform: ${name} (POSIX: fork() death tests)\n")
        set(MANIFEST "${MANIFEST}" PARENT_SCOPE)
        return()
    endif()
    list(TRANSFORM T_SOURCES PREPEND ${TESTS}/)
    add_executable(${name} ${T_SOURCES} ${WIN32_MANIFEST_RC})   # WIN32_MANIFEST_RC: the UTF-8 code page manifest, Windows only (empty elsewhere)
    target_compile_options(${name} PRIVATE ${HOST_FLAGS})
    target_compile_definitions(${name} PRIVATE ${T_DEFS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${T_INCLUDES})
    target_link_libraries(${name} PRIVATE nxplat)
    if(T_PROFILE)
        add_dependencies(${name} game_profile)
        target_include_directories(${name} PRIVATE ${GEN} ${PROFILE_DIR})
    endif()
    if(T_VPX)
        target_link_libraries(${name} PRIVATE vpx)
        target_include_directories(${name} PRIVATE ${REPO}/dm/third_party/stb_vorbis)
    endif()
    if(T_SDL)
        target_link_libraries(${name} PRIVATE SDL3::SDL3-static)
    endif()
    if(NOT T_RUNS)
        set(T_RUNS -)
    endif()
    set(tmp ${CMAKE_BINARY_DIR}/tmp)
    foreach(run IN LISTS T_RUNS)
        set(test_name ${name})
        set(args)
        if(NOT run STREQUAL "-")
            set(test_name ${name}${run})
            set(args ${run})
        endif()
        add_test(NAME ${test_name} COMMAND ${name} ${args} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set(tmpdir -)
        if(T_TMPDIR)
            file(MAKE_DIRECTORY ${tmp})
            set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "TMPDIR=${tmp}")
            set(tmpdir tmpdir)
        endif()
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
        string(JOIN " " argv ${args})
        if(argv STREQUAL "")
            set(argv -)
        endif()
        string(APPEND MANIFEST "${test_name}\t${name}${CMAKE_EXECUTABLE_SUFFIX}\t${argv}\t${tmpdir}\n")
    endforeach()
    set(MANIFEST "${MANIFEST}" PARENT_SCOPE)
endfunction()

host_test(t_motion SOURCES t_motion.c)
host_test(t_motion_pro SOURCES t_motion_pro.c)
host_test(t_haptics SOURCES t_haptics.c ../hle_haptics.c)
host_test(t_vorbis SOURCES t_vorbis.c)
host_test(t_vorbis_divides SOURCES t_vorbis.c DEFS STB_VORBIS_DIVIDES_IN_RESIDUE)
host_test(t_touch SOURCES t_touch.c ../hle_touch.c PROFILE)
host_test(t_gesture SOURCES t_gesture.c ../hle_touch.c PROFILE)
host_test(t_thread_leak SOURCES t_thread_leak.c PROFILE)
host_test(t_swkbd SOURCES t_swkbd.c ../hle_swkbd.c PROFILE)
host_test(t_pctl SOURCES t_pctl.c PROFILE)
host_test(t_time SOURCES t_time.c PROFILE)
host_test(t_hid SOURCES t_hid.c ../hle_haptics.c PROFILE)
host_test(t_audio_spsc SOURCES t_audio_spsc.c PROFILE SDL)
host_test(t_audio_sdl SOURCES t_audio_sdl.c PROFILE SDL)
host_test(t_front_sdl SOURCES t_front_sdl.c ../hle_haptics.c PROFILE SDL)
host_test(t_audren_work SOURCES t_audren_work.c PROFILE)
host_test(t_nro SOURCES t_nro.c PROFILE)
host_test(t_fs SOURCES t_fs.c t_fs_stub.c PROFILE TMPDIR)
host_test(t_fs_commit_list SOURCES t_fs_commit_list.c t_fs_stub.c PROFILE TMPDIR)
host_test(t_fs_case SOURCES t_fs_case.c PROFILE TMPDIR)
host_test(t_condvar SOURCES t_condvar.c t_condvar_stub.c ../hle_thread.c PROFILE DEFS DM_TEST_HOOKS)
host_test(t_condvar_old SOURCES t_condvar.c t_condvar_stub.c ../hle_thread.c PROFILE DEFS DM_TEST_HOOKS DM_TEST_CV_OLD_ORDER)
host_test(t_preempt SOURCES t_preempt.c t_condvar_stub.c PROFILE RUNS - --all-peers)
host_test(t_notify_sched SOURCES t_notify_sched.c t_condvar_stub.c PROFILE DEFS DM_TEST_THREAD_ENTER RUNS - --all-peers)
host_test(t_movie_vpx SOURCES t_movie_vpx.c PROFILE VPX)
# The NVN replay's draw and render-target state and its GPU-write ledger (dm/gfx/nvn, plain C; dm/gfx/tests/Makefile builds the same two on the Mac)
host_test(t_state SOURCES ../../gfx/tests/t_state.c ../../gfx/nvn/nvn_state.c INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include)
host_test(t_gpuwrite SOURCES ../../gfx/tests/t_gpuwrite.c ../../gfx/nvn/nvn_gpuwrite.c INCLUDES ${REPO}/dm/gfx/nvn)
# The host-call thunks and the host-function page: the generator's output for the synthetic entries of t_hle_thunks_entries.c, which t_hle_thunks.c includes
# (its death tests are skipped on Windows)
set(T_HLE_THUNKS_C ${CMAKE_BINARY_DIR}/gen/t_hle_thunks.c)
add_custom_command(OUTPUT ${T_HLE_THUNKS_C}
    COMMAND ${Python3_EXECUTABLE} ${GEN_HLE_THUNKS} -o ${T_HLE_THUNKS_C} ${TESTS}/t_hle_thunks_entries.c
    DEPENDS ${GEN_HLE_THUNKS} ${TESTS}/t_hle_thunks_entries.c ${HOST_H}
    COMMENT "host-call thunks of the test entries" VERBATIM)
add_custom_target(t_hle_thunks_gen DEPENDS ${T_HLE_THUNKS_C})
set_source_files_properties(${TESTS}/t_hle_thunks.c PROPERTIES OBJECT_DEPENDS ${T_HLE_THUNKS_C})
host_test(t_hle_thunks SOURCES t_hle_thunks.c t_hle_thunks_entries.c ../aot_hostfn.c PROFILE INCLUDES ${CMAKE_BINARY_DIR} ${REPO}/dm/tools/aot/include)
add_dependencies(t_hle_thunks t_hle_thunks_gen)
# fork() death tests
host_test(t_heap_size SOURCES t_heap_size.c PROFILE POSIX)
host_test(t_oe SOURCES t_oe.c PROFILE POSIX)
host_test(t_fpslock SOURCES t_fpslock.c PROFILE POSIX)
host_test(t_audren_layout SOURCES t_audren_layout.c PROFILE POSIX)
host_test(t_shared_font SOURCES t_shared_font.c PROFILE POSIX)
host_test(t_system_event SOURCES t_system_event.c PROFILE POSIX)
host_test(t_symbols SOURCES t_symbols.c PROFILE POSIX)
host_test(t_svc_memory SOURCES t_svc_memory.c POSIX INCLUDES ${TESTS}/memory_profile)
host_test(t_svc_memory_unlimited SOURCES t_svc_memory.c POSIX INCLUDES ${TESTS}/memory_profile DEFS DM_GAME_MEM_MAP_LIMIT=0)
host_test(t_svc_memory_16k SOURCES t_svc_memory.c POSIX INCLUDES ${TESTS}/memory_profile DEFS TEST_PAGE_16K)
# Not portable yet (Apple only, dm/host/tests/Makefile): t_altstack (crash.c's signal stack: native execution), t_audio and t_audio_device (the Core Audio render callback,
# device and AudioUnit watchdog; t_audio_sdl checks the SDL3 output here), t_motion_apple, t_motion_pro_mac and t_haptics_apple (Apple frontends: GameController, IOKit,
# Core Haptics; t_front_sdl checks the SDL3 front end here), t_svc_memory_dirty (the Metal backend's write-protect page tracking, dm/gfx/nvn)

file(WRITE ${CMAKE_BINARY_DIR}/host_tests.tsv "${MANIFEST}")
