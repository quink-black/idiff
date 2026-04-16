cmake_minimum_required(VERSION 3.20)
project(idiff VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type defaults to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Find pkg-config for libraries without CMake configs
find_package(PkgConfig REQUIRED)

# --- System-required dependencies (too complex to build from source) ---

# OpenCV
find_package(OpenCV REQUIRED COMPONENTS core imgproc quality)

# ImageMagick (pkg-config)
# ImageMagick depends on many third-party libraries (libjpeg, libpng, libtiff,
# libwebp, freetype, fontconfig, etc.) and uses autotools — not practical to
# build from source via CMake.
pkg_check_modules(MAGICKXX REQUIRED IMPORTED_TARGET Magick++)

# SDL2
# SDL2 has complex platform-specific backends; system package is preferred.
find_package(SDL2 REQUIRED)

# --- Dependencies with source-build fallback ---

# Catch2 v3
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found on system — will build from source")
    set(IDIFF_BUILD_CATCH2 ON)
else()
    message(STATUS "Found system Catch2: ${Catch2_DIR}")
    set(IDIFF_BUILD_CATCH2 OFF)
endif()

# Native File Dialog Extended
find_package(nfd QUIET)
if(NOT nfd_FOUND)
    message(STATUS "nfd not found on system — will build from source")
    set(IDIFF_BUILD_NFD ON)
else()
    message(STATUS "Found system nfd: ${nfd_DIR}")
    set(IDIFF_BUILD_NFD OFF)
endif()

# LibRaw — try pkg-config first, then build from source.
# All paths ensure the target PkgConfig::LIBRAW is available for consumers.
pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw)
if(LIBRAW_FOUND)
    message(STATUS "Found system LibRaw via pkg-config")
    set(IDIFF_BUILD_LIBRAW OFF)
else()
    message(STATUS "LibRaw not found on system — will build from source")
    set(IDIFF_BUILD_LIBRAW ON)
endif()

add_subdirectory(third_party)
add_subdirectory(src)

option(IDIFF_BUILD_TESTS "Build unit tests" ON)
if(IDIFF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
