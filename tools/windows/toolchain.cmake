# CMake settings for building for 64-bit Windows on Linux with llvm-mingw (Clang, the MinGW-w64
# headers and the Universal C Runtime that comes with Windows 10 and 11). build_libs.sh unpacks
# the compiler into OpenXcom/build-win64/deps/toolchain; the libraries go to deps/prefix, which is
# the only place (with the compiler's own Windows files) libraries and headers are looked for.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The compiler, from this file's place in tools/windows
get_filename_component(OXC3D_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OXC3D_DEPS "${OXC3D_ROOT}/OpenXcom/build-win64/deps")
set(CMAKE_C_COMPILER "${OXC3D_DEPS}/toolchain/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${OXC3D_DEPS}/toolchain/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${OXC3D_DEPS}/toolchain/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR "${OXC3D_DEPS}/toolchain/bin/x86_64-w64-mingw32-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${OXC3D_DEPS}/toolchain/bin/x86_64-w64-mingw32-ranlib" CACHE FILEPATH "")

# Windows libraries and headers only: the compiler's and the ones built for the game
set(CMAKE_FIND_ROOT_PATH "${OXC3D_DEPS}/toolchain/x86_64-w64-mingw32" "${OXC3D_DEPS}/prefix")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
