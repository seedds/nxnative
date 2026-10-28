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

# ---- The portable host sources ----
# Not here: the parts tied to native ARM64 execution (entry.S, hle_bigstack.c, crash.c, probe.c; the x86-64 execution engine, Phase 2, replaces them), the program entry
# main.c (it runs a frontend), and the Apple frontend files (*.m). hle_fdstat.c compiles to nothing on Windows
file(GLOB HLE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hle_*.c)
list(FILTER HLE_SRC EXCLUDE REGEX "/hle_bigstack\\.c$")
add_library(nxhost STATIC hle.c loader.c thread.c ${HLE_SRC})
add_dependencies(nxhost game_profile)
target_compile_options(nxhost PRIVATE ${HOST_FLAGS})
target_include_directories(nxhost PUBLIC ${GEN} ${PROFILE_DIR} ${REPO}/dm/third_party/stb_vorbis)
target_link_libraries(nxhost PUBLIC nxplat vpx)

# ---- Unit tests (dm/host/tests: each includes the host sources it exercises, so a test links the platform layer, not nxhost) ----
# host_test(<name> SOURCES <files> [PROFILE] [VPX] [POSIX] [DEFS <macros>] [RUNS <arguments; one run each, "-" = none>] [TMPDIR])
#   PROFILE: needs the game profile header; POSIX: needs fork() (death tests in a child), so Linux only for now
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
    cmake_parse_arguments(T "PROFILE;VPX;POSIX;TMPDIR" "" "SOURCES;DEFS;RUNS;INCLUDES" ${ARGN})
    if(T_POSIX AND WIN32)
        string(APPEND MANIFEST "# not built for this platform: ${name} (POSIX: fork() death tests)\n")
        set(MANIFEST "${MANIFEST}" PARENT_SCOPE)
        return()
    endif()
    list(TRANSFORM T_SOURCES PREPEND ${TESTS}/)
    add_executable(${name} ${T_SOURCES})
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
host_test(t_audio_spsc SOURCES t_audio_spsc.c PROFILE)
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
# device and AudioUnit watchdog), t_motion_apple, t_motion_pro_mac and t_haptics_apple (Apple frontends: GameController, IOKit, Core Haptics), t_svc_memory_dirty (the
# Metal backend's write-protect page tracking, dm/gfx/nvn)

file(WRITE ${CMAKE_BINARY_DIR}/host_tests.tsv "${MANIFEST}")
