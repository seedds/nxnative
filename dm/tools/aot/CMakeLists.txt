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
set(NXAOT_LLVM_COMPONENTS core support orcjit native nativecodegen asmparser asmprinter passes analysis mc)

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

# The census (src/analysis): instruction classes and the phase 0 questions over a raw code blob
add_library(nxaot_analysis STATIC src/analysis/census.cpp)
target_link_libraries(nxaot_analysis PUBLIC nxaot_decode)
target_compile_options(nxaot_analysis PRIVATE ${NXAOT_WARNINGS})

# The lifter (src/lift): dynarmic IR -> LLVM IR, and the ORC JIT that runs lifted blocks in nxaot_vec
add_library(nxaot_lift STATIC src/lift/lifter.cpp src/lift/opcodes.cpp src/lift/jit.cpp)
target_include_directories(nxaot_lift PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_include_directories(nxaot_lift SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
separate_arguments(_llvm_defs NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(nxaot_lift PUBLIC ${_llvm_defs})
target_link_libraries(nxaot_lift PUBLIC nxaot_decode)
target_compile_options(nxaot_lift PRIVATE ${NXAOT_WARNINGS})
if (NXAOT_LLVM_STATIC OR NOT LLVM_LINK_LLVM_DYLIB)
    llvm_map_components_to_libnames(_llvm_libs ${NXAOT_LLVM_COMPONENTS})
    target_link_libraries(nxaot_lift PUBLIC ${_llvm_libs})
else()
    llvm_config(nxaot_lift USE_SHARED ${NXAOT_LLVM_COMPONENTS})
endif()

add_executable(nxaot src/main.cpp)
target_link_libraries(nxaot PRIVATE nxaot_analysis nxaot_lift)
target_compile_options(nxaot PRIVATE ${NXAOT_WARNINGS})

add_executable(nxaot_vec src/vec/main.cpp src/vec/vectors.cpp src/vec/reference.cpp src/vec/lifted.cpp)
target_link_libraries(nxaot_vec PRIVATE nxaot_lift)
target_compile_options(nxaot_vec PRIVATE ${NXAOT_WARNINGS})

# Tests: the vectors (lifted against dynarmic), the census over the generator's synthetic blob, and the committed vectors against the generator
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME vectors COMMAND nxaot_vec "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
add_test(NAME census COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_census.py" --nxaot $<TARGET_FILE:nxaot> --work "${CMAKE_CURRENT_BINARY_DIR}/census")
add_test(NAME vectors_current COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_vectors.py" --check)
set_tests_properties(vectors census PROPERTIES TIMEOUT 600)
