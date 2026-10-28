# Cross-build for Windows 10+ (x86-64) with llvm-mingw (clang, lld, the UCRT and winpthreads): cmake -DCMAKE_TOOLCHAIN_FILE=dm/host/cmake/x86_64-w64-mingw32.cmake ...
# The compilers are found on PATH (the mstorsjo/llvm-mingw image puts them in /opt/llvm-mingw/bin; on Windows, llvm-mingw's bin folder). C++ only for SDL3's Windows sources
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_AR x86_64-w64-mingw32-ar CACHE FILEPATH "")
set(CMAKE_RANLIB x86_64-w64-mingw32-ranlib CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
