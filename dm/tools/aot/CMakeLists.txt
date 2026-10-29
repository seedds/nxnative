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
set(NXAOT_LLVM_COMPONENTS core support orcjit native nativecodegen asmparser asmprinter passes analysis mc transformutils)

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
find_package(LLD REQUIRED CONFIG HINTS "${LLVM_LIBRARY_DIR}/cmake/lld" "${LLVM_DIR}/../lld")
add_library(nxaot_build STATIC src/build/plan.cpp src/build/pack.cpp)
target_include_directories(nxaot_build SYSTEM PRIVATE ${LLD_INCLUDE_DIRS})
target_link_libraries(nxaot_build PUBLIC nxaot_analysis nxaot_lift lldELF lldCommon)
target_compile_options(nxaot_build PRIVATE ${NXAOT_WARNINGS})

add_executable(nxaot src/main.cpp)
target_link_libraries(nxaot PRIVATE nxaot_analysis nxaot_lift nxaot_build)
target_compile_options(nxaot PRIVATE ${NXAOT_WARNINGS})

# The native run time of a pack (src/rt, aot-recompiler.md sections 2.4, 3.3, 4, 5, 9.4): no LLVM code generation, the pack's shared object
# loaded with dlopen; the library resolves the host's imports and the floating-point helpers against the executable (ENABLE_EXPORTS)
add_library(nxaot_rt STATIC src/rt/host.cpp)
target_include_directories(nxaot_rt PUBLIC include src)
target_link_libraries(nxaot_rt PUBLIC nxaot_module ${CMAKE_DL_LIBS})
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
# against its expected output and the dynarmic run, with the dispatcher's lookup-table and unknown-target checks; and every function of every
# NRO against dynarmic from the same snapshot (section 12, item 2)
add_test(NAME corpus_native COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_corpus_native.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-host $<TARGET_FILE:nxaot_host> --nxaot-run $<TARGET_FILE:nxaot_run> --work "${CMAKE_CURRENT_BINARY_DIR}/native")
add_test(NAME corpus_funcdiff COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_funcdiff.py" --nxaot $<TARGET_FILE:nxaot>
         --nxaot-funcdiff $<TARGET_FILE:nxaot_funcdiff> --work "${CMAKE_CURRENT_BINARY_DIR}/funcdiff")
set_tests_properties(vectors vectors_v3 vectors_perop opcodes_reach census discover manifest corpus_ref corpus_native corpus_funcdiff PROPERTIES TIMEOUT 900)
