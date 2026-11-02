# nxaot, the ahead-of-time ARM64 -> x86-64 recompiler (docs/design/aot-recompiler.md), and nxaot_vec, its differential vector harness against dynarmic.
# README.md has the commands (Docker image docker/Dockerfile.aot). Build trees go to build/ at the repository root, which git ignores.
cmake_minimum_required(VERSION 3.20)
project(nxaot LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(NXAOT_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
cmake_path(NORMAL_PATH NXAOT_REPO)
set(NXAOT_DYNARMIC_DIR "${NXAOT_REPO}/dm/third_party/dynarmic")

# The lock file: the LLVM release and the dynarmic commit this tree is built and tested with. A different LLVM stops the configure step, as a different
# translator stops a shader build (AGENTS.md, dmsh_chain.lock)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/nxaot.lock" _lock_rows REGEX "^[a-z]+\t")
foreach(_row IN LISTS _lock_rows)
    string(REPLACE "\t" ";" _fields "${_row}")
    list(GET _fields 0 _key)
    list(GET _fields 1 _value)
    set(NXAOT_LOCK_${_key} "${_value}")
endforeach()
if (NOT NXAOT_LOCK_llvm OR NOT NXAOT_LOCK_dynarmic)
    message(FATAL_ERROR "nxaot.lock: the llvm and dynarmic rows are required")
endif()

find_package(LLVM ${NXAOT_LOCK_llvm} EXACT REQUIRED CONFIG)
if (NOT LLVM_PACKAGE_VERSION VERSION_EQUAL NXAOT_LOCK_llvm)
    message(FATAL_ERROR "LLVM ${LLVM_PACKAGE_VERSION} found at ${LLVM_DIR}, nxaot.lock pins ${NXAOT_LOCK_llvm}: use the image of docker/Dockerfile.aot")
endif()
message(STATUS "nxaot: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR}), as nxaot.lock pins")

file(STRINGS "${NXAOT_DYNARMIC_DIR}/VENDOR.md" _vendor_commit REGEX "^\\| Commit \\| `[0-9a-f]+`")
string(REGEX MATCH "[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]+" _vendor_commit "${_vendor_commit}")
if (NOT _vendor_commit STREQUAL NXAOT_LOCK_dynarmic)
    message(FATAL_ERROR "dm/third_party/dynarmic/VENDOR.md names commit '${_vendor_commit}', nxaot.lock pins ${NXAOT_LOCK_dynarmic}")
endif()
configure_file(src/nxaot_lock.h.in "${CMAKE_CURRENT_BINARY_DIR}/gen/nxaot_lock.h" @ONLY)

# LLVM: the shared libLLVM where the package was built with one (the Ubuntu packages of the image are), else the static component libraries.
# NXAOT_LLVM_STATIC=ON links the components statically, as the distributed nxaot will be (aot-recompiler.md section 13)
option(NXAOT_LLVM_STATIC "Link the LLVM component libraries statically instead of libLLVM" OFF)
set(NXAOT_LLVM_COMPONENTS core support orcjit native nativecodegen asmparser asmprinter passes analysis mc transformutils mcdisassembler
    ${LLVM_NATIVE_ARCH}disassembler object)

# dynarmic: the A64 frontend (the lifter's semantics) and the x86-64 backend (the reference executor of nxaot_vec). Its own tests, the A32 frontend and its
# precompiled headers are off; warnings of vendored code are not ours to fix
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
add_subdirectory("${NXAOT_DYNARMIC_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dynarmic" EXCLUDE_FROM_ALL)
find_package(Boost 1.57 REQUIRED)

set(NXAOT_WARNINGS -Wall -Wextra)

# The decoder glue (src/decode): dynarmic's A64 translation loop with the encodings it leaves to an interpreter that nxaot translates itself
add_library(nxaot_decode STATIC src/decode/translate.cpp)
target_include_directories(nxaot_decode PUBLIC include src)
target_link_libraries(nxaot_decode PUBLIC dynarmic merry::mcl fmt::fmt tsl::robin_map Boost::boost)
target_compile_options(nxaot_decode PRIVATE ${NXAOT_WARNINGS})

# The analyses (src/analysis): the census of phase 0 over a raw code blob, and over a whole module (aot-recompiler.md section 1.3) function
# discovery, CFGs, jump tables on the lifted IR (hence the lifter and LLVM's analyses), exclusive windows and the interprocedural summaries
add_library(nxaot_analysis STATIC src/analysis/census.cpp src/analysis/reach.cpp src/analysis/discover.cpp src/analysis/jumptable.cpp
            src/analysis/summary.cpp)
target_link_libraries(nxaot_analysis PUBLIC nxaot_decode nxaot_module nxaot_lift)
target_compile_options(nxaot_analysis PRIVATE ${NXAOT_WARNINGS})

separate_arguments(_llvm_defs NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# The module loader (src/module): NROs and behaviour images with their manifests (aot-recompiler.md sections 1.1 to 1.4); LLVM's SHA-256
add_library(nxaot_module STATIC src/module/image.cpp src/module/manifest.cpp)
target_include_directories(nxaot_module PUBLIC src)
target_include_directories(nxaot_module SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(nxaot_module PRIVATE ${_llvm_defs})
target_compile_options(nxaot_module PRIVATE ${NXAOT_WARNINGS})
if (NXAOT_LLVM_STATIC OR NOT LLVM_LINK_LLVM_DYLIB)
    llvm_map_components_to_libnames(_llvm_support_libs support)
    target_link_libraries(nxaot_module PUBLIC ${_llvm_support_libs})
else()
    llvm_config(nxaot_module USE_SHARED support)
endif()

# The lifter (src/lift): dynarmic IR -> LLVM IR, and the ORC JIT that runs lifted blocks in nxaot_vec
# The floating-point helpers lifted code calls (src/lift/runtime.cpp): objects of nxaot_lift (its JIT defines them) and of every executable that
# runs a pack (the pack's shared object resolves them against the executable)
add_library(nxaot_runtime OBJECT src/lift/runtime.cpp)
target_include_directories(nxaot_runtime PUBLIC include src)
target_link_libraries(nxaot_runtime PUBLIC dynarmic merry::mcl fmt::fmt tsl::robin_map Boost::boost)
target_compile_options(nxaot_runtime PRIVATE ${NXAOT_WARNINGS})

add_library(nxaot_lift STATIC src/lift/lifter.cpp src/lift/lower_fp.cpp src/lift/lower_vector.cpp src/lift/lower_crypto.cpp src/lift/lower_calls.cpp
            src/lift/opcodes.cpp src/lift/jit.cpp)
target_include_directories(nxaot_lift PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_include_directories(nxaot_lift SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(nxaot_lift PUBLIC ${_llvm_defs})
target_link_libraries(nxaot_lift PUBLIC nxaot_decode PRIVATE nxaot_runtime)
target_compile_options(nxaot_lift PRIVATE ${NXAOT_WARNINGS})
if (NXAOT_LLVM_STATIC OR NOT LLVM_LINK_LLVM_DYLIB)
    llvm_map_components_to_libnames(_llvm_libs ${NXAOT_LLVM_COMPONENTS})
    target_link_libraries(nxaot_lift PUBLIC ${_llvm_libs})
else()
    llvm_config(nxaot_lift USE_SHARED ${NXAOT_LLVM_COMPONENTS})
endif()

# The pack stage (src/build, aot-recompiler.md sections 9.2 to 9.4): whole modules in function mode, the tables, the descriptor
# (include/nxaot/aot_module.h), the tier's passes, the object, and lld as a library for the shared object (section 13)
# Compile units on worker threads, one LLVM context each, and the per-function cache of section 13 (src/cache: objects by the SHA-256 of
# what each depends on, LLVM's SHA-256)
find_package(LLD REQUIRED CONFIG HINTS "${LLVM_LIBRARY_DIR}/cmake/lld" "${LLVM_DIR}/../lld")
find_package(Threads REQUIRED)
# A Windows pack (--target windows, README.md "Windows packs"): the same ELF objects joined, converted to COFF (src/build/coff.cpp) and linked by lld's
# COFF driver into a DLL
# The tables' object's rows go to the MC layer directly (src/build/tables.cpp; README.md, "The tables' object through MC")
add_library(nxaot_build STATIC src/build/plan.cpp src/build/pack.cpp src/build/tables.cpp src/build/coff.cpp src/build/analysis_cache.cpp
            src/cache/cache.cpp)
target_include_directories(nxaot_build SYSTEM PRIVATE ${LLD_INCLUDE_DIRS})
target_link_libraries(nxaot_build PUBLIC nxaot_analysis nxaot_lift lldELF lldCOFF lldCommon Threads::Threads)
target_compile_options(nxaot_build PRIVATE ${NXAOT_WARNINGS})

add_executable(nxaot src/main.cpp)
target_link_libraries(nxaot PRIVATE nxaot_analysis nxaot_lift nxaot_build)
target_compile_options(nxaot PRIVATE ${NXAOT_WARNINGS})

# The native run time of a pack (src/rt, aot-recompiler.md sections 2.4, 3.3, 4, 5, 9.4): no LLVM code generation, the pack's shared object
# loaded with dlopen; the library resolves the host's imports and the floating-point helpers against the executable (ENABLE_EXPORTS)
# The JIT fallback (src/rt/jit.cpp, aot-recompiler.md section 11) is dynarmic's A64 JIT behind the C interface of the design; guest threads are
# pthreads (section 6.1)
find_package(Threads REQUIRED)
add_library(nxaot_rt STATIC src/rt/host.cpp src/rt/jit.cpp)
target_include_directories(nxaot_rt PUBLIC include src)
target_link_libraries(nxaot_rt PUBLIC nxaot_module nxaot_decode dynarmic Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(nxaot_rt PRIVATE ${NXAOT_WARNINGS})
add_executable(nxaot_host src/rt/host_main.cpp)
target_link_libraries(nxaot_host PRIVATE nxaot_rt nxaot_runtime)
set_target_properties(nxaot_host PROPERTIES ENABLE_EXPORTS ON)
target_compile_options(nxaot_host PRIVATE ${NXAOT_WARNINGS})
# nxaot_funcdiff: every function of a pack against dynarmic from the same snapshot (aot-recompiler.md section 12, item 2)
add_executable(nxaot_funcdiff src/rt/funcdiff.cpp)
target_link_libraries(nxaot_funcdiff PRIVATE nxaot_rt nxaot_runtime dynarmic)
set_target_properties(nxaot_funcdiff PROPERTIES ENABLE_EXPORTS ON)
target_compile_options(nxaot_funcdiff PRIVATE ${NXAOT_WARNINGS})

# zlib reads the compressed vector files (tests/vectors/*.tsv.gz); LLVM itself is built with it (LLVM_ENABLE_ZLIB), so it is no new dependency
find_package(ZLIB REQUIRED)
add_executable(nxaot_vec src/vec/main.cpp src/vec/vectors.cpp src/vec/reference.cpp src/vec/lifted.cpp)
target_link_libraries(nxaot_vec PRIVATE nxaot_lift ZLIB::ZLIB)
target_compile_options(nxaot_vec PRIVATE ${NXAOT_WARNINGS})

# nxaot_run: a corpus NRO under dynarmic's A64 JIT with the corpus's two SVCs (the reference output of dm/tools/aot/corpus)
add_executable(nxaot_run src/run/main.cpp)
target_link_libraries(nxaot_run PRIVATE nxaot_module nxaot_lift)
target_compile_options(nxaot_run PRIVATE ${NXAOT_WARNINGS})

# nxaot_bench: the cost of the NaN-chain checks and the x86 code of lifted floating point (README.md, "Measurements")
add_executable(nxaot_bench src/bench/main.cpp src/bench/calls.cpp)
target_link_libraries(nxaot_bench PRIVATE nxaot_lift nxaot_build nxaot_rt nxaot_runtime)
target_compile_definitions(nxaot_bench PRIVATE NXAOT_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
set_target_properties(nxaot_bench PROPERTIES ENABLE_EXPORTS ON)
target_compile_options(nxaot_bench PRIVATE ${NXAOT_WARNINGS})

# Tests: the vectors (lifted against dynarmic) on this machine's CPU; the cryptographic vectors for x86-64-v3, which has no AES-NI, SHA or
# PCLMULQDQ (their software forms); the scalar floating point and the sequences with every operation checking its own NaNs (NaNMode::PerOp,
# the reference of the chains); every opcode the A64 frontend emits lowered; the census over the generator's synthetic blob; and the committed
# vectors against the generator
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_vectors "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
add_test(NAME vectors COMMAND nxaot_vec "${_vectors}")
add_test(NAME vectors_v3 COMMAND nxaot_vec --cpu x86-64-v3 "${_vectors}/crypto.tsv.gz")
add_test(NAME vectors_perop COMMAND nxaot_vec --nan-mode perop "${_vectors}/fp.tsv.gz" "${_vectors}/fp_seq.tsv.gz")
add_test(NAME opcodes_reach COMMAND nxaot opcodes --reach --check)
add_test(NAME census COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_census.py" --nxaot $<TARGET_FILE:nxaot> --work "${CMAKE_CURRENT_BINARY_DIR}/census")
add_test(NAME vectors_current COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_vectors.py" --check)
# The module loader, discovery and the corpus (aot-recompiler.md sections 1.1 to 1.4 and 12, item 3): discovery of every corpus NRO against its
# ELF's symbol table, the manifest parser and the behaviour-image form, the corpus under dynarmic against its expected output, the fp cases current
add_test(NAME discover COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_discover.py" --nxaot $<TARGET_FILE:nxaot> --work "${CMAKE_CURRENT_BINARY_DIR}/discover")
add_test(NAME manifest COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_manifest.py" --nxaot $<TARGET_FILE:nxaot> --work "${CMAKE_CURRENT_BINARY_DIR}/manifest")
add_test(NAME corpus_ref COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_corpus_run.py" --nxaot-run $<TARGET_FILE:nxaot_run>)
add_test(NAME corpus_fp_current COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/corpus/gen_fp.py" --check)
# Phase 1 (aot-recompiler.md section 14): the corpus recompiled and run natively in both conventions, the correctness mode and the max mode,
# against its expected output and the dynarmic run, with the dispatcher's lookup-table check; and every function of every
# NRO against dynarmic from the same snapshot (section 12, item 2)
add_test(NAME corpus_native COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_corpus_native.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-host $<TARGET_FILE:nxaot_host> --nxaot-run $<TARGET_FILE:nxaot_run> --work "${CMAKE_CURRENT_BINARY_DIR}/native")
add_test(NAME corpus_funcdiff COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_funcdiff.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-funcdiff $<TARGET_FILE:nxaot_funcdiff> --work "${CMAKE_CURRENT_BINARY_DIR}/funcdiff")
# Phase 3 (aot-recompiler.md sections 3.3, 3.4, 6 and 11): every NRO natively, with everything in the JIT and with no JIT, and the dispatcher's
# miss into the JIT; the threaded programs many times on host threads and in the JIT; the pause test of section 6.2 on them
add_test(NAME corpus_jit COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_corpus_jit.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-host $<TARGET_FILE:nxaot_host> --work "${CMAKE_CURRENT_BINARY_DIR}/jit")
add_test(NAME corpus_threads COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_threads.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-host $<TARGET_FILE:nxaot_host> --work "${CMAKE_CURRENT_BINARY_DIR}/threads")
add_test(NAME safepoints COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_safepoints.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-host $<TARGET_FILE:nxaot_host> --work "${CMAKE_CURRENT_BINARY_DIR}/safepoints")
# Phase 5 (aot-recompiler.md sections 10 and 13): the per-function cache: a cold pack of every NRO misses every function, a warm one hits every
# function and gives the same shared object byte for byte; a changed instruction word misses its function only, a changed tier every function
add_test(NAME cache COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cache.py" --nxaot $<TARGET_FILE:nxaot>
         --work "${CMAKE_CURRENT_BINARY_DIR}/cache")
# What a call site reloads after an indirect call: AAPCS64's result registers whatever the module's own functions write (a host function or
# another pack may be the callee)
add_test(NAME plan COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_plan.py" --nxaot $<TARGET_FILE:nxaot>
         --work "${CMAKE_CURRENT_BINARY_DIR}/plan")
# The line table (aot-recompiler.md section 3.5): every entry of every corpus pack and of the synthetic modules' names an x86 load or store of its
# function's body and the A64 load or store of that function's guest code it was lifted from (nxaot lines)
add_test(NAME lines COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lines.py" --nxaot $<TARGET_FILE:nxaot>
         --work "${CMAKE_CURRENT_BINARY_DIR}/lines")
# Phase 5b (aot-recompiler.md section 9.3): functions split for size, run: the corpus with the split limit lowered so that nearly every function
# runs as parts (both conventions, the correctness mode, the line table of the parts), and switch-gcc's sw_word at the default limit
add_test(NAME split COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_split.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-host $<TARGET_FILE:nxaot_host> --work "${CMAKE_CURRENT_BINARY_DIR}/split")
# Tiers by profile (section 9.3): a profile written by hand, sampled functions at tier H (H+ in the max mode) and the others at C (H), the tier
# counts and the native run
add_test(NAME profile COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_profile.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-host $<TARGET_FILE:nxaot_host> --work "${CMAKE_CURRENT_BINARY_DIR}/profile")
# Windows packs (README.md "Windows packs", aot-recompiler.md 9.4): switch-gcc, fp-gcc and the synthetic modules as DLLs, checked with LLVM's
# llvm-readobj and llvm-objdump against their ELF packs: a PE DLL for x86-64 that imports from no module, its exports, its descriptor and tables,
# its import table and thunks, its base relocations, its line table, the code function by function, determinism; a call of anything else refused
add_test(NAME pack_windows COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pack_windows.py" --nxaot $<TARGET_FILE:nxaot>
         --readobj "${LLVM_TOOLS_BINARY_DIR}/llvm-readobj" --objdump "${LLVM_TOOLS_BINARY_DIR}/llvm-objdump" --work "${CMAKE_CURRENT_BINARY_DIR}/pack_windows")
set_tests_properties(vectors vectors_v3 vectors_perop opcodes_reach census discover manifest corpus_ref corpus_native corpus_funcdiff corpus_jit
                     corpus_threads safepoints cache plan lines split profile pack_windows PROPERTIES TIMEOUT 900)
# Phase 5c (aot-recompiler.md sections 1.3 and 10): the analysis on threads gives the analysis on one thread, byte for byte: discovery's TSV
# and lines, the plan, the analysis cache's entry and the pack, with 1 and 8 threads, for every corpus NRO and the synthetic modules
add_test(NAME analysis_threads COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_analysis_threads.py"
         --nxaot $<TARGET_FILE:nxaot> --work "${CMAKE_CURRENT_BINARY_DIR}/analysis_threads")
set_tests_properties(analysis_threads PROPERTIES TIMEOUT 900)
# Phase 5d (aot-recompiler.md section 9.3): every function of every NRO at tier C against dynarmic from the same snapshot (the code FastISel
# selects, with the IR tier C prepares for it: README.md, "Phase 5d"); corpus_native runs the corpus at tier C too
add_test(NAME corpus_funcdiff_c COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_funcdiff.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-funcdiff $<TARGET_FILE:nxaot_funcdiff> --tier C --work "${CMAKE_CURRENT_BINARY_DIR}/funcdiff_c")
set_tests_properties(corpus_funcdiff_c PROPERTIES TIMEOUT 900)
# The host's JIT misses as discovery's input (aot-recompiler.md 1.3, 3.3 and 11; README.md, "JIT misses"): the synthetic module's hidden function, in a data block,
# compiled when a misses file in the host's form names it (its gate served, the caches' keys), and a corpus function held from discovery's roots brought back
add_test(NAME misses COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_misses.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-host $<TARGET_FILE:nxaot_host> --work "${CMAKE_CURRENT_BINARY_DIR}/misses")
set_tests_properties(misses PROPERTIES TIMEOUT 900)
# The compile workers bounded by memory (aot-recompiler.md section 10; README.md, "Memory at scale"): --workers 0 counts them by the memory
# available after the analysis (NXAOT_MEM_AVAILABLE_MIB, a test switch, stands for it), --workers N overrides, and the pack is the same
add_test(NAME workers COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workers.py" --nxaot $<TARGET_FILE:nxaot>
         --work "${CMAKE_CURRENT_BINARY_DIR}/workers")
set_tests_properties(workers PROPERTIES TIMEOUT 900)
# Gap code (aot-recompiler.md 1.3 "Coverage" and 9.3; README.md, "Gap code at tier C"): a game-shaped module with a quarter of its functions
# without an FDE (gen_bench.py --direct --no-fde-percent 25), each found with its exact bounds; the functions only the gap sweep found at the
# mode's tier by default and at tier C with --gap-tier C (--tiers-tsv), the same packs with 1 and 4 workers and warm, a sampled one at the
# profile's tier, none at C in the max mode
add_test(NAME gaps COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gaps.py" --nxaot $<TARGET_FILE:nxaot>
         --work "${CMAKE_CURRENT_BINARY_DIR}/gaps")
set_tests_properties(gaps PROPERTIES TIMEOUT 900)
# The tables' object through MC (aot-recompiler.md 9.4 and 10; README.md, "The tables' object through MC"): the function map's and the lookup
# table's rows written by the MC layer give the same tables' object and the same pack as the rows as LLVM constants (NXAOT_TABLES_IR, a test
# switch), over corpus NROs, the synthetic game module and a 3,000-function game-shaped module, at tiers C, W and H+, for Linux and Windows
add_test(NAME tables COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tables.py" --nxaot $<TARGET_FILE:nxaot>
         --work "${CMAKE_CURRENT_BINARY_DIR}/tables")
set_tests_properties(tables PROPERTIES TIMEOUT 900)
