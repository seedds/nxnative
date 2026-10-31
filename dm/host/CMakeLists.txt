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
# Not here: the parts tied to native ARM64 execution (entry.S, hle_bigstack.c, crash.c, probe.c; the x86-64 execution engine replaces them: aot_*.c below, aot_fault.c
# for crash.c's fault hooks), the program entry main.c (it runs a frontend), and the Apple frontend files (*.m). front_sdl.c is the front end here. hle_fdstat.c
# compiles to nothing on Windows
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

# ---- The NVN frontend (docs/design/vulkan-backend.md, step M6) ----
# The NVN interface layer (hle_nvn.c, with its dirty-page tracker and texture layout) and the plain C parts of the replay every graphics backend shares: the draw state, the GPU-write
# ledger, handle resolution, the pool table, the decoder, the shader reflection parser (over cJSON) and the pipeline description, with dm/gfx/tex's deswizzling and format table.
# They reach a backend only through dm/gfx/nvn/nvn_be.h. Part of nxhost, so the thunk generator reads their entries (NVN( and NVN_NOPROTO(, hle_nvn.c; NVN's own header through
# -I). Apple builds keep dm/host/Makefile and the Metal backend; here nxhost links the Vulkan one (dm/gfx/hostvk, the nvn_vk library below; NXHOST_VULKAN=OFF: nvn_null).
# nvn_names.c: the NVN names in load order, written from names.txt (nvn_thunks.S holds them on Apple)
set(NVN_FRONTEND_SRC
    ${REPO}/dm/gfx/nvn/hle_nvn.c ${REPO}/dm/gfx/nvn/nvn_dirty.c ${REPO}/dm/gfx/nvn/nvn_tex.c ${REPO}/dm/gfx/nvn/nvn_state.c ${REPO}/dm/gfx/nvn/nvn_gpuwrite.c
    ${REPO}/dm/gfx/nvn/nvn_resolve.c ${REPO}/dm/gfx/nvn/nvn_pools.c ${REPO}/dm/gfx/nvn/nvn_replay.c ${REPO}/dm/gfx/nvn/nvn_refl.c ${REPO}/dm/gfx/nvn/nvn_pipe.c
    ${REPO}/dm/gfx/tex/nvn_fmt_mtl.c ${REPO}/dm/gfx/tex/nvn_deswizzle.c ${REPO}/dm/third_party/cjson/cJSON.c)
set_source_files_properties(${NVN_FRONTEND_SRC} PROPERTIES INCLUDE_DIRECTORIES ${REPO}/dm/gfx/nvn/include)
set(NVN_NAMES_C ${CMAKE_BINARY_DIR}/gen/nvn_names.c)
file(STRINGS ${REPO}/dm/gfx/nvn/names.txt NVN_NAMES REGEX "^nvn")
list(LENGTH NVN_NAMES NVN_NNAMES)
set(nvn_names_text "// Generated by dm/host/CMakeLists.txt from dm/gfx/nvn/names.txt: the NVN names in load order (nvn_thunks.S holds them on Apple); do not edit\n")
string(APPEND nvn_names_text "#include \"${REPO}/dm/gfx/nvn/nvn_count.h\"\nconst char *nvn_names[NVN_NFUNC] = {\n")
foreach(name IN LISTS NVN_NAMES)
    string(APPEND nvn_names_text "    \"${name}\",\n")
endforeach()
string(APPEND nvn_names_text "};\n_Static_assert(${NVN_NNAMES} == NVN_NFUNC, \"names.txt and nvn_count.h count different names: run dm/gfx/tools/gen_protos.py\");\n")
file(GENERATE OUTPUT ${NVN_NAMES_C} CONTENT "${nvn_names_text}")
set_source_files_properties(${NVN_NAMES_C} PROPERTIES GENERATED TRUE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${REPO}/dm/gfx/nvn/names.txt)
list(APPEND NXHOST_SRC ${NVN_FRONTEND_SRC} ${NVN_NAMES_C})
file(GLOB NVN_H CONFIGURE_DEPENDS ${REPO}/dm/gfx/nvn/*.h ${REPO}/dm/gfx/nvn/include/*.h ${REPO}/dm/gfx/tex/*.h)
list(APPEND HOST_H ${NVN_H})
set(GEN_HLE_THUNKS_INC -I ${REPO}/dm/gfx/nvn/include)
# The null backend (dm/gfx/nvn/nvn_null.c): every backend function as a red line on its first call and a neutral answer, for a host binary with no GPU backend yet; a library of its
# own, so that a binary links exactly one backend (nxhost takes the Vulkan one unless NXHOST_VULKAN=OFF; t_frontend this one)
add_library(nvn_null STATIC ${REPO}/dm/gfx/nvn/nvn_null.c)
target_compile_options(nvn_null PRIVATE ${HOST_FLAGS})
target_include_directories(nvn_null PRIVATE ${REPO}/dm/gfx/nvn/include)
target_link_libraries(nvn_null PUBLIC nxplat)

add_custom_command(OUTPUT ${HLE_THUNKS_C}
    COMMAND ${Python3_EXECUTABLE} ${GEN_HLE_THUNKS} -o ${HLE_THUNKS_C} ${GEN_HLE_THUNKS_INC} ${NXHOST_SRC}
    DEPENDS ${GEN_HLE_THUNKS} ${NXHOST_SRC} ${HOST_H}
    COMMENT "host-call thunks from the HLE entries" VERBATIM)
# The x86-64 execution engine (docs/design/aot-recompiler.md; dm/host/aot.h): the run time of a pack's recompiled code in the host. Its arena below 2^39, module
# images and packs (aot_arena.c), what a pack imports and dm_guest_call (aot_run.c), guest threads (aot_thread.c), faults (aot_fault.c) and the floating-point
# helpers (aot_fp.c). A pack's library resolves those imports against the executable that links them (ENABLE_EXPORTS)
set(AOT_SRC aot_hostfn.c aot_arena.c aot_run.c aot_thread.c aot_fault.c aot_fp.c)
add_library(nxhost STATIC ${NXHOST_SRC} ${AOT_SRC} ${HLE_THUNKS_C})
add_dependencies(nxhost game_profile)
target_compile_options(nxhost PRIVATE ${HOST_FLAGS})
target_include_directories(nxhost PUBLIC ${GEN} ${PROFILE_DIR} ${REPO}/dm/third_party/stb_vorbis ${REPO}/dm/tools/aot/include)
target_link_libraries(nxhost PUBLIC nxplat vpx SDL3::SDL3-static)
# The JIT fallback of the engine (design section 11): dynarmic's A64 JIT behind the C interface of dm/tools/aot/src/rt/jit.h, the glue nxaot's own run time uses
# (jit.cpp, with the decoder glue it calls). dynarmic needs C++20 and Boost's headers, which the nxnative-aot image has and nxnative-build does not, so it is an
# option, off by default: without it, guest code with no compiled entry stops the process with a [RED] line and status 3 (aot.h DM_AOT_JIT_NEEDED)
option(NXHOST_JIT "x86-64 execution engine: build the JIT fallback (dynarmic; needs C++20 and Boost's headers)" OFF)
if(NXHOST_JIT)
    enable_language(CXX)
    find_package(Boost 1.57)
    if(NOT Boost_FOUND)
        message(FATAL_ERROR "NXHOST_JIT needs Boost's headers (dynarmic includes boost/icl and boost/variant): build in the nxnative-aot image (docker/Dockerfile.aot)")
    endif()
    set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
    set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
    add_subdirectory(${REPO}/dm/third_party/dynarmic ${CMAKE_BINARY_DIR}/dynarmic EXCLUDE_FROM_ALL)
    add_library(nxhost_jit STATIC ${REPO}/dm/tools/aot/src/rt/jit.cpp ${REPO}/dm/tools/aot/src/decode/translate.cpp)
    set_target_properties(nxhost_jit PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(nxhost_jit PUBLIC ${REPO}/dm/tools/aot/include ${REPO}/dm/tools/aot/src)
    target_link_libraries(nxhost_jit PUBLIC dynarmic merry::mcl fmt::fmt tsl::robin_map Boost::boost)
    target_compile_definitions(nxhost PUBLIC NXHOST_JIT=1)
    target_link_libraries(nxhost PUBLIC nxhost_jit)
endif()

# ---- Unit tests (dm/host/tests: each includes the host sources it exercises, so a test links the platform layer, not nxhost) ----
# host_test(<name> SOURCES <files> [PROFILE] [VPX] [SDL] [POSIX] [DEFS <macros>] [RUNS <arguments; one run each, "-" = none>] [TMPDIR] [ENV <VAR=value ...>])
#   PROFILE: needs the game profile header; SDL: links SDL3 (front_sdl.c, hle_audio.c); POSIX: needs fork() (death tests in a child), so Linux only for now;
#   ENV: the environment of the CTest run only (host_tests.tsv does not carry it, so the runners do not set it: the test must also run without it)
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
    cmake_parse_arguments(T "PROFILE;VPX;SDL;POSIX;TMPDIR" "" "SOURCES;DEFS;RUNS;INCLUDES;ENV" ${ARGN})
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
        set(env ${T_ENV})
        if(T_TMPDIR)
            file(MAKE_DIRECTORY ${tmp})
            list(APPEND env "TMPDIR=${tmp}")
            set(tmpdir tmpdir)
        endif()
        if(env)
            set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${env}")
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
host_test(t_condvar SOURCES t_condvar.c t_condvar_stub.c ../hle_thread.c PROFILE DEFS DM_TEST_HOOKS INCLUDES ${REPO}/dm/tools/aot/include)
host_test(t_condvar_old SOURCES t_condvar.c t_condvar_stub.c ../hle_thread.c PROFILE DEFS DM_TEST_HOOKS DM_TEST_CV_OLD_ORDER INCLUDES ${REPO}/dm/tools/aot/include)
host_test(t_preempt SOURCES t_preempt.c t_condvar_stub.c PROFILE RUNS - --all-peers INCLUDES ${REPO}/dm/tools/aot/include)
host_test(t_notify_sched SOURCES t_notify_sched.c t_condvar_stub.c PROFILE DEFS DM_TEST_THREAD_ENTER RUNS - --all-peers INCLUDES ${REPO}/dm/tools/aot/include)
host_test(t_movie_vpx SOURCES t_movie_vpx.c PROFILE VPX)
# The NVN replay's draw and render-target state and its GPU-write ledger (dm/gfx/nvn, plain C; dm/gfx/tests/Makefile builds the same two on the Mac)
host_test(t_state SOURCES ../../gfx/tests/t_state.c ../../gfx/nvn/nvn_state.c INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include)
host_test(t_gpuwrite SOURCES ../../gfx/tests/t_gpuwrite.c ../../gfx/nvn/nvn_gpuwrite.c INCLUDES ${REPO}/dm/gfx/nvn)
# Its handle resolution and memory pool table (the same, step M2b)
host_test(t_resolve SOURCES ../../gfx/tests/t_resolve.c ../../gfx/nvn/nvn_resolve.c ../../gfx/nvn/nvn_state.c ../../gfx/nvn/nvn_gpuwrite.c INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include)
host_test(t_pools SOURCES ../../gfx/tests/t_pools.c ../../gfx/nvn/nvn_pools.c INCLUDES ${REPO}/dm/gfx/nvn)
# Its decoder of command lists over a stub backend (step M3)
host_test(t_replay SOURCES ../../gfx/tests/t_replay.c ../../gfx/nvn/nvn_replay.c ../../gfx/nvn/nvn_state.c INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include)
# Its neutral pipeline description and the prewarm lists' pipeline rows (step M5): t_pipe reads every profile's pso_manifest.tsv, finding the repository by walking up from
# the build folder (DM_REPO overrides); the inverse of the NVN to Metal format table beside it (dm/gfx/tex)
host_test(t_pipe SOURCES ../../gfx/tests/t_pipe.c ../../gfx/nvn/nvn_pipe.c ../../gfx/tex/nvn_fmt_mtl.c INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include ${REPO}/dm/gfx/tex)
host_test(t_fmt_inv SOURCES ../../gfx/tex/tests/t_fmt_inv.c ../../gfx/tex/nvn_fmt_mtl.c INCLUDES ${REPO}/dm/gfx/tex)
# The shader reflection parser in plain C, with its JSON reader (dm/gfx/nvn/nvn_refl.c, dm/third_party/cjson): every backend's since step M6, the Metal one's too
host_test(t_refl_c SOURCES ../../gfx/tests/t_refl_c.c ../../gfx/nvn/nvn_refl.c ../../third_party/cjson/cJSON.c INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/third_party/cjson)
# The Vulkan backend's device layer and host-memory import (dm/gfx/hostvk; docs/design/vulkan-backend.md 3.6, 4.2, 9.1), over volk: the Vulkan loader is loaded at run time,
# so nothing links against libvulkan or vulkan-1.dll (dl, which volk needs on Linux, comes with the platform layer). Headless. Without a Vulkan loader or device they print
# "note: no Vulkan device: skipped" and pass (the Windows CI runner has no Vulkan driver); CTest on Linux runs them on Mesa's lavapipe under the validation layer, both in the
# nxnative-build image (docker/README.md), and then a missing driver or layer fails them
set(HOSTVK_SRC ../../gfx/hostvk/vk_device.c ../../gfx/hostvk/vk_import.c ../../third_party/volk/volk.c)
set(HOSTVK_INC ${REPO}/dm/gfx/hostvk ${REPO}/dm/third_party/volk ${REPO}/dm/third_party/vulkan-headers/include)
set(HOSTVK_ENV)
if(NOT WIN32)
    set(HOSTVK_ENV VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json VK_LOADER_DRIVERS_SELECT=lvp* VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation)
endif()
host_test(t_vk_device SOURCES ../../gfx/hostvk/tests/t_vk_device.c ${HOSTVK_SRC} PROFILE DEFS VK_NO_PROTOTYPES INCLUDES ${HOSTVK_INC} ENV ${HOSTVK_ENV})
host_test(t_vk_import SOURCES ../../gfx/hostvk/tests/t_vk_import.c ${HOSTVK_SRC} PROFILE DEFS VK_NO_PROTOTYPES INCLUDES ${HOSTVK_INC} ENV ${HOSTVK_ENV})
# The Vulkan backend behind dm/gfx/nvn/nvn_be.h (dm/gfx/hostvk; docs/design/vulkan-backend.md, step P3): a library of its own beside nvn_null, so that a binary links exactly one
# backend (nxhost links it: NXHOST_VULKAN below). Its own shaders are SPIR-V arrays in committed headers (dm/gfx/hostvk/shaders/gen/, written by shaders/gen_spirv.sh), so building needs no glslang
set(HOSTVK_BACKEND_SRC vk_backend.c vk_memory.c vk_texture.c vk_shader.c vk_pipeline.c vk_commands.c vk_device.c vk_import.c vk_gpl.c vk_cache.c vk_prewarm.c)   # the last three: phase P5
list(TRANSFORM HOSTVK_BACKEND_SRC PREPEND ${REPO}/dm/gfx/hostvk/)
list(APPEND HOSTVK_BACKEND_SRC ${REPO}/dm/gfx/tex/nvn_fmt_vk.c ${REPO}/dm/gfx/tex/astc_decode.c ${REPO}/dm/third_party/volk/volk.c)   # astc_decode.c: the CPU fallback of the ASTC decode (P7)
add_library(nvn_vk STATIC ${HOSTVK_BACKEND_SRC})
add_dependencies(nvn_vk game_profile)
target_compile_options(nvn_vk PRIVATE ${HOST_FLAGS})
target_compile_definitions(nvn_vk PUBLIC VK_NO_PROTOTYPES)
target_include_directories(nvn_vk PUBLIC ${HOSTVK_INC} PRIVATE ${GEN} ${PROFILE_DIR} ${REPO}/dm/gfx/nvn/include)
target_link_libraries(nvn_vk PUBLIC nxplat)
# The graphics backend nxhost links (step M7, done at step P8): the Vulkan one on Linux and Windows, which takes the null backend's forms itself when no Vulkan device meets the
# baseline of design 9.1 (vk_backend.c: one red line, nothing drawn, the CPU side runs); OFF links the null backend (nvn_null). t_frontend keeps the null backend either way. The
# device comes up at the frontend's first call that needs it (a memory pool, a texture, a submission), as the Metal backend's mtl_init does: the host calls no be_init of its own.
# The backend and the frontend in nxhost reference each other, so a binary links the pair twice (LINK_INTERFACE_MULTIPLICITY)
option(NXHOST_VULKAN "nxhost links the Vulkan graphics backend (nvn_vk) instead of the null one (nvn_null)" ON)
if(NXHOST_VULKAN)
    target_link_libraries(nxhost PUBLIC nvn_vk)
else()
    target_link_libraries(nxhost PUBLIC nvn_null)
endif()
set_target_properties(nxhost PROPERTIES LINK_INTERFACE_MULTIPLICITY 2)
# The guest Vulkan layer on that backend (dm/gfx/hostvk/guest_vk.c; docs/design/vulkan-backend.md 8.1, step P9a): the game's vk* imports forwarded to the backend's device. A
# library of its own beside nvn_vk, because it is a game-facing API layer like hle_nvn.c (import-table entries, host services: the frame tick, the vi layer crop, the graphics
# gate), not part of the NVN backend that the GPU tests link; nxhost links it with the Vulkan backend. Its VK( / VKB( entries register in the import table and get host-call
# thunks like the HLE entries: the thunk generator's command above runs again over nxhost's sources and the shim's, with the Vulkan headers on its include path, and its file is
# the one compiled (APPEND keeps this change beside the backend it belongs to; folded into that command, the first run goes away). The thunk table references every entry's C
# function, so linking it pulls the shim's object, and its entries, into a host binary
set(GUEST_VK_SRC ${REPO}/dm/gfx/hostvk/guest_vk.c)
add_library(guest_vk STATIC ${GUEST_VK_SRC})
add_dependencies(guest_vk game_profile)
target_compile_options(guest_vk PRIVATE ${HOST_FLAGS})
target_include_directories(guest_vk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${GEN} ${PROFILE_DIR} ${REPO}/dm/gfx/nvn/include)
target_link_libraries(guest_vk PUBLIC nvn_vk)
if(NXHOST_VULKAN)
    target_link_libraries(nxhost PUBLIC guest_vk)
    add_custom_command(OUTPUT ${HLE_THUNKS_C} APPEND
        COMMAND ${Python3_EXECUTABLE} ${GEN_HLE_THUNKS} -o ${HLE_THUNKS_C} ${GEN_HLE_THUNKS_INC} -I ${REPO}/dm/third_party/vulkan-headers/include ${NXHOST_SRC} ${GUEST_VK_SRC}
        DEPENDS ${GUEST_VK_SRC} ${REPO}/dm/gfx/hostvk/guest_vk.h)
endif()
# Its GPU tests (dm/gfx/tests/vk; t_vk_harness.h says what they share): the NVN frontend, as objects so that the backend's references to it resolve whatever the link order,
# over nvn_vk, each drawing headless and reading pixels back. Without a Vulkan device they skip as t_vk_device does; CTest on Linux runs them on lavapipe under the validation layer
add_library(nvn_front_obj OBJECT ${NVN_FRONTEND_SRC} ${NVN_NAMES_C})
add_dependencies(nvn_front_obj game_profile)
target_compile_options(nvn_front_obj PRIVATE ${HOST_FLAGS})
target_include_directories(nvn_front_obj PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${GEN} ${PROFILE_DIR} ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include)
target_link_libraries(nvn_front_obj PUBLIC nxplat)
foreach(t IN ITEMS queue_origin clear_ds clear_tex depth16_clear integer_blend primitive_restart rast_discard vbuf readback view_swizzle queries cond_render copy_alias)
    host_test(t_vk_${t} SOURCES ../../gfx/tests/vk/t_vk_${t}.c PROFILE INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include ${REPO}/dm/gfx/tests/vk TMPDIR ENV ${HOSTVK_ENV})
    target_link_libraries(t_vk_${t} PRIVATE nvn_front_obj nvn_vk)
endforeach()
# The descriptor microbenchmark (design 2.3): cached sets with dynamic offsets against push descriptors over 4,900 draws a frame, a table of CPU times. Without the validation
# layer (its cost would be what is measured), so its CTest environment names the driver only
set(HOSTVK_ENV_BENCH)
if(NOT WIN32)
    set(HOSTVK_ENV_BENCH VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json VK_LOADER_DRIVERS_SELECT=lvp*)
endif()
# The NVN → Vulkan format table against NVN's block sizes and the formats' names (plain C, no Vulkan)
host_test(t_fmt_vk SOURCES ../../gfx/tex/tests/t_fmt_vk.c ../../gfx/tex/nvn_fmt_vk.c ../../gfx/nvn/nvn_tex.c INCLUDES ${REPO}/dm/gfx/tex ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include ${REPO}/dm/third_party/vulkan-headers/include)
host_test(t_vk_descbench SOURCES ../../gfx/hostvk/tests/t_vk_descbench.c ${HOSTVK_SRC} PROFILE DEFS VK_NO_PROTOTYPES INCLUDES ${HOSTVK_INC} ENV ${HOSTVK_ENV_BENCH})
# The committed SPIR-V headers rebuild byte for byte (gen_spirv.sh --check): where glslang and spirv-val are installed, in the version the headers name (the nxnative-build
# image); skipped elsewhere (exit 77). CTest only: the runners' list has no shell scripts
if(NOT WIN32)
    add_test(NAME t_vk_spirv COMMAND sh ${REPO}/dm/gfx/hostvk/shaders/gen_spirv.sh --check)
    set_tests_properties(t_vk_spirv PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    string(APPEND MANIFEST "# CTest only: t_vk_spirv (dm/gfx/hostvk/shaders/gen_spirv.sh --check)\n")
endif()
# Pipelines without stutter (dm/gfx/hostvk vk_gpl.c, vk_cache.c, vk_prewarm.c; docs/design/vulkan-backend.md section 3, step P5), on lavapipe under the validation layer as the
# GPU tests above, skipped without a Vulkan device: t_vk_gpl_identity (the same draws through fast-linked, optimized, level-B, level-C and level-D pipelines, bit for bit),
# t_vk_pipeline_cache (the cache file written, read back at a second start, invalidated, a corrupt file ignored) and t_vk_prewarm (a manifest of about 200 rows prewarmed in the
# background and with the prebuild, cold and warm). The last two run themselves again as child processes for each start (t_vk_harness.h hv_spawn)
foreach(t IN ITEMS gpl_identity pipeline_cache prewarm)
    host_test(t_vk_${t} SOURCES ../../gfx/tests/vk/t_vk_${t}.c PROFILE INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include ${REPO}/dm/gfx/tests/vk TMPDIR ENV ${HOSTVK_ENV})
    target_link_libraries(t_vk_${t} PRIVATE nvn_front_obj nvn_vk)
endforeach()
# Textures (docs/design/vulkan-backend.md section 5, step P7): t_astc, the ASTC decoder in plain C (dm/gfx/tex/astc_decode.c) over blocks it writes and predicts (every host, no
# Vulkan); on lavapipe under the validation layer as the GPU tests above: t_vk_astc (synthetic ASTC of every block size held natively, decoded by the compute shader and decoded by
# the C decoder: the compute decode equal to the C one bit for bit, lavapipe's own decode compared), t_vk_formats (every row of the format table with a path, sampled and drawn,
# through the device's formats and through every substitute), t_vk_depth24 (DEPTH24 and DEPTH24_STENCIL8 as D24 and as 32-bit float with the bias of decision D10, uploads,
# copies and partial clears of the depth-stencil formats, stencil views) and t_vk_deswizzle (the GPU deswizzle against the CPU one, bit for bit, with both timed)
host_test(t_astc SOURCES ../../gfx/tex/tests/t_astc.c ../../gfx/tex/astc_decode.c INCLUDES ${REPO}/dm/gfx/tex ${REPO}/dm/gfx/tex/tests)
# t_astc_mesa: the same decoder against an independent one, Mesa's, through desktop OpenGL on llvmpipe (lavapipe's Vulkan device has no ASTC). CTest only, on Linux where EGL
# and GL are found (the nxnative-build image); it exits 77 (skipped) without a GL context or the ASTC extension. Test-only: nothing of the host links EGL or GL
if(NOT WIN32)
    find_path(EGL_INCLUDE EGL/egl.h)
    find_library(EGL_LIB EGL)
    find_library(GL_LIB GL)
    if(EGL_INCLUDE AND EGL_LIB AND GL_LIB)
        add_executable(t_astc_mesa ${REPO}/dm/gfx/tex/tests/t_astc_mesa.c ${REPO}/dm/gfx/tex/astc_decode.c)
        target_compile_options(t_astc_mesa PRIVATE ${HOST_FLAGS})
        target_include_directories(t_astc_mesa PRIVATE ${EGL_INCLUDE} ${REPO}/dm/gfx/tex/tests)
        target_link_libraries(t_astc_mesa PRIVATE ${EGL_LIB} ${GL_LIB})
        add_test(NAME t_astc_mesa COMMAND t_astc_mesa)
        set_tests_properties(t_astc_mesa PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        string(APPEND MANIFEST "# CTest only: t_astc_mesa (EGL and GL: the ASTC decoder against Mesa's)\n")
    endif()
endif()
foreach(t IN ITEMS astc formats depth24 deswizzle)
    host_test(t_vk_${t} SOURCES ../../gfx/tests/vk/t_vk_${t}.c PROFILE INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include ${REPO}/dm/gfx/tests/vk ${REPO}/dm/gfx/tex TMPDIR ENV ${HOSTVK_ENV})
    target_link_libraries(t_vk_${t} PRIVATE nvn_front_obj nvn_vk)
endforeach()
# The native features Metal lacks (docs/design/vulkan-backend.md section 6, step P8), on lavapipe under the validation layer as the GPU tests above, skipped without a Vulkan
# device: t_vk_transform_feedback (the Metal test's captures, pauses, resumes, three streams, a geometry capture and DrawTransformFeedback, plus indexed and instanced captures
# and two capturing pipelines in one pass), t_vk_geometry (the Metal test's geometry expansion over array, indexed, indirect and GPU-written draws, a line strip with adjacency
# widened into quads, and layered rendering from the geometry and the vertex stage), t_vk_tessellation (quad and triangle domains, the triangles made counted by a capture),
# t_vk_feedback (draws that sample their own attachment, the pass split when the pass wrote it) and t_vk_raster (Bresenham lines on levels A and D, advanced blend modes,
# indirect quads, an array of descriptor handles, draw-based copies); t_vk_queries above covers the counters of step P8
foreach(t IN ITEMS transform_feedback geometry tessellation feedback raster)
    host_test(t_vk_${t} SOURCES ../../gfx/tests/vk/t_vk_${t}.c PROFILE INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include ${REPO}/dm/gfx/tests/vk TMPDIR ENV ${HOSTVK_ENV})
    target_link_libraries(t_vk_${t} PRIVATE nvn_front_obj nvn_vk)
endforeach()
# The guest Vulkan layer (docs/design/vulkan-backend.md 8.1, step P9a): t_vk_guest drives guest_vk.c through the entry points a game gets from its vkGetInstanceProcAddr (found by
# name in the import table, as the loader binds them) on lavapipe under the validation layer, skipped without a Vulkan device as the GPU tests above: the Tegra shape, memory,
# buffers and images, render passes, pipelines (the two SPIR-V fix-ups among them), draws read back, queries, and a swapchain presented with a layer crop through the backend's
# headless presenter; t_vk_spirv_fix runs the SPIR-V checks and fix-ups on hand-written modules (no device; spirv-val checks what they produce where it is installed)
foreach(t IN ITEMS guest spirv_fix)
    host_test(t_vk_${t} SOURCES ../../gfx/tests/vk/t_vk_${t}.c PROFILE INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include ${REPO}/dm/gfx/tests/vk TMPDIR ENV ${HOSTVK_ENV})
    target_link_libraries(t_vk_${t} PRIVATE nvn_front_obj guest_vk nvn_vk)
endforeach()
# The NVN frontend linked over the null backend (step M6): its entry points called as C functions, a device, a queue and a memory pool made, a command list recorded, submitted and
# handed by the decoder to the null backend (dm/gfx/tests/t_frontend.c). Here only: on Apple nvn_be.h's functions are the Metal backend's
host_test(t_frontend SOURCES ../../gfx/tests/t_frontend.c ../../gfx/nvn/hle_nvn.c ../../gfx/nvn/nvn_dirty.c ../../gfx/nvn/nvn_tex.c ../../gfx/nvn/nvn_state.c ../../gfx/nvn/nvn_pools.c
          ../../gfx/nvn/nvn_replay.c ../../gfx/tex/nvn_deswizzle.c PROFILE INCLUDES ${REPO}/dm/gfx/nvn ${REPO}/dm/gfx/nvn/include)
target_sources(t_frontend PRIVATE ${NVN_NAMES_C})
target_link_libraries(t_frontend PRIVATE nvn_null)
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
# The x86-64 execution engine in the host on the corpus of dm/tools/aot (t_aot_host.c): every corpus NRO mapped by the host's loader, its pack loaded, _start run
# through dm_aot_run with the host's SVC path, its lines compared with corpus/expected; excl's threads on hle_thread.c, its pause test through SetThreadActivity
# and GetThreadContext3; a guest call into the host-function page; a write-protected data segment resumed by a fault hook. Each run is a child process. The packs
# come from dm/tools/aot/tests/build_packs.sh (the nxnative-aot image): this option, or the environment variable NXAOT_PACKS when it is empty; with neither the
# test prints a note and passes (the Windows runner, a tree built without the packs). Its own host functions get thunks from the generator, as the host's do
set(NXAOT_PACKS "" CACHE PATH "folder of the corpus packs (dm/tools/aot/tests/build_packs.sh) that t_aot_host runs; empty: the environment variable NXAOT_PACKS")
set(T_AOT_HOST_THUNKS_C ${CMAKE_BINARY_DIR}/gen/t_aot_host_thunks.c)
add_custom_command(OUTPUT ${T_AOT_HOST_THUNKS_C}
    COMMAND ${Python3_EXECUTABLE} ${GEN_HLE_THUNKS} -o ${T_AOT_HOST_THUNKS_C} ${TESTS}/t_aot_host_entries.c
    DEPENDS ${GEN_HLE_THUNKS} ${TESTS}/t_aot_host_entries.c ${HOST_H}
    COMMENT "host-call thunks of t_aot_host's entries" VERBATIM)
set(AOT_TEST_SRC)
foreach(f IN LISTS AOT_SRC ITEMS hle_svc.c hle_thread.c thread.c loader.c)
    list(APPEND AOT_TEST_SRC ../${f})
endforeach()
host_test(t_aot_host SOURCES t_aot_host.c t_aot_host_entries.c t_aot_host_stub.c ${AOT_TEST_SRC} PROFILE INCLUDES ${REPO}/dm/tools/aot/include)
target_sources(t_aot_host PRIVATE ${T_AOT_HOST_THUNKS_C})
target_compile_definitions(t_aot_host PRIVATE "T_AOT_CORPUS=\"${REPO}/dm/tools/aot/corpus\"" "T_AOT_PACKS=\"${NXAOT_PACKS}\"")
set_target_properties(t_aot_host PROPERTIES ENABLE_EXPORTS ON)   # a pack's library resolves dm_aot_svc, nxaot_rt_* and the floating-point helpers against it
if(NXHOST_JIT)
    target_compile_definitions(t_aot_host PRIVATE NXHOST_JIT=1)
    target_link_libraries(t_aot_host PRIVATE nxhost_jit)
endif()
set_tests_properties(t_aot_host PROPERTIES TIMEOUT 600)
# A game module through the engine's game-module path (t_aot_module.c): the synthetic module of dm/tools/tests/aot_synth.py (no game files), its behaviour image and
# manifest as patch_nso.py --aot writes them, packed by build_packs.sh into <packs>/module; loader.c's load_modules maps it, registers its gates, binds its imports into
# its GOT (a host function of t_aot_module_entries.c, an unimplemented import) and loads its pack; the module runs through dm_aot_run, and its SetHeapSize reaches the
# stub backend's be_guest_map; an image that is not its manifest's and a pack of another image are refused. The same packs as t_aot_host (without them, a note and a pass)
set(T_AOT_MODULE_THUNKS_C ${CMAKE_BINARY_DIR}/gen/t_aot_module_thunks.c)
add_custom_command(OUTPUT ${T_AOT_MODULE_THUNKS_C}
    COMMAND ${Python3_EXECUTABLE} ${GEN_HLE_THUNKS} -o ${T_AOT_MODULE_THUNKS_C} ${TESTS}/t_aot_module_entries.c
    DEPENDS ${GEN_HLE_THUNKS} ${TESTS}/t_aot_module_entries.c ${HOST_H}
    COMMENT "host-call thunks of t_aot_module's entries" VERBATIM)
host_test(t_aot_module SOURCES t_aot_module.c t_aot_module_entries.c t_aot_module_stub.c ${AOT_TEST_SRC} ../hle.c PROFILE INCLUDES ${REPO}/dm/tools/aot/include)
target_sources(t_aot_module PRIVATE ${T_AOT_MODULE_THUNKS_C})
target_compile_definitions(t_aot_module PRIVATE "T_AOT_PACKS=\"${NXAOT_PACKS}\"")
set_target_properties(t_aot_module PROPERTIES ENABLE_EXPORTS ON)   # the pack's imports resolve against it, as against nxhost
if(NXHOST_JIT)
    target_compile_definitions(t_aot_module PRIVATE NXHOST_JIT=1)
    target_link_libraries(t_aot_module PRIVATE nxhost_jit)
endif()
set_tests_properties(t_aot_module PROPERTIES TIMEOUT 300)
# The memory SVCs' announcements to the graphics backend (hle_svc.c be_guest_map / be_guest_unmap; docs/design/vulkan-backend.md 4.2), recorded by a stub backend on
# the small layout of t_svc_memory: which SVCs commit and decommit, the ranges, and that an unmapped range was still readable when announced
host_test(t_guest_map SOURCES t_guest_map.c INCLUDES ${TESTS}/memory_profile)

# fork() death tests
host_test(t_heap_size SOURCES t_heap_size.c PROFILE POSIX)
host_test(t_oe SOURCES t_oe.c PROFILE POSIX)
host_test(t_fpslock SOURCES t_fpslock.c PROFILE POSIX)
host_test(t_audren_layout SOURCES t_audren_layout.c PROFILE POSIX)
host_test(t_shared_font SOURCES t_shared_font.c PROFILE POSIX)
host_test(t_system_event SOURCES t_system_event.c PROFILE POSIX INCLUDES ${REPO}/dm/tools/aot/include)
host_test(t_symbols SOURCES t_symbols.c PROFILE POSIX)
host_test(t_svc_memory SOURCES t_svc_memory.c POSIX INCLUDES ${TESTS}/memory_profile)
host_test(t_svc_memory_unlimited SOURCES t_svc_memory.c POSIX INCLUDES ${TESTS}/memory_profile DEFS DM_GAME_MEM_MAP_LIMIT=0)
host_test(t_svc_memory_16k SOURCES t_svc_memory.c POSIX INCLUDES ${TESTS}/memory_profile DEFS TEST_PAGE_16K)
# Not portable yet (Apple only, dm/host/tests/Makefile): t_altstack (crash.c's signal stack: native execution), t_audio and t_audio_device (the Core Audio render callback,
# device and AudioUnit watchdog; t_audio_sdl checks the SDL3 output here), t_motion_apple, t_motion_pro_mac and t_haptics_apple (Apple frontends: GameController, IOKit,
# Core Haptics; t_front_sdl checks the SDL3 front end here), t_svc_memory_dirty (the dirty-page tracker, dm/gfx/nvn/nvn_dirty.c, under real faults: it needs host pages larger than the guest's 4 KB, as Apple's 16 KB)

file(WRITE ${CMAKE_BINARY_DIR}/host_tests.tsv "${MANIFEST}")
