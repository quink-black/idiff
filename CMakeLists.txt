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

# --- Options ---

# ImageMagick is the preferred loader (ICC profiles, wide format coverage,
# color-space awareness) but is unavailable or awkward to install on some
# platforms (notably vcpkg / Windows, and some Linux CI images).  OpenCV
# imgcodecs is always available and always compiled in as the fallback.
#
# Values:
#   AUTO (default): use ImageMagick if pkg-config finds it, otherwise skip.
#   ON:             require ImageMagick; fail configuration if missing.
#   OFF:            skip ImageMagick entirely, OpenCV-only build.
set(IDIFF_WITH_IMAGEMAGICK "AUTO" CACHE STRING
    "Enable ImageMagick loader backend: AUTO | ON | OFF")
set_property(CACHE IDIFF_WITH_IMAGEMAGICK PROPERTY STRINGS AUTO ON OFF)

# --- System-required dependencies ---

# OpenCV -- imgcodecs is used both as a general-image loader backend and as
# the writer for the "Save viewport" feature, so it is always required.
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs quality)

# ImageMagick (pkg-config) -- optional, controlled by IDIFF_WITH_IMAGEMAGICK.
# ImageMagick depends on many third-party libraries (libjpeg, libpng,
# libtiff, libwebp, freetype, fontconfig, etc.) and uses autotools -- not
# practical to build from source via CMake.
set(IDIFF_HAVE_IMAGEMAGICK OFF)
if(NOT IDIFF_WITH_IMAGEMAGICK STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        if(IDIFF_WITH_IMAGEMAGICK STREQUAL "ON")
            pkg_check_modules(MAGICKXX REQUIRED IMPORTED_TARGET Magick++)
        else()
            pkg_check_modules(MAGICKXX QUIET IMPORTED_TARGET Magick++)
        endif()
        if(MAGICKXX_FOUND)
            set(IDIFF_HAVE_IMAGEMAGICK ON)
            message(STATUS "ImageMagick (Magick++) found -- "
                           "enabling ImageMagick loader backend")
        endif()
    elseif(IDIFF_WITH_IMAGEMAGICK STREQUAL "ON")
        message(FATAL_ERROR
                "IDIFF_WITH_IMAGEMAGICK=ON but pkg-config is not available")
    endif()
endif()

if(NOT IDIFF_HAVE_IMAGEMAGICK)
    message(STATUS
        "ImageMagick loader backend disabled -- OpenCV imgcodecs will be "
        "the only image loader backend")
endif()

# SDL2
find_package(SDL2 REQUIRED CONFIG)

# --- Dependencies with source-build fallback ---

# Catch2 v3
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found on system -- will build from source")
    set(IDIFF_BUILD_CATCH2 ON)
else()
    message(STATUS "Found system Catch2: ${Catch2_DIR}")
    set(IDIFF_BUILD_CATCH2 OFF)
endif()

# Native File Dialog Extended
find_package(nfd QUIET)
if(NOT nfd_FOUND)
    message(STATUS "nfd not found on system -- will build from source")
    set(IDIFF_BUILD_NFD ON)
else()
    message(STATUS "Found system nfd: ${nfd_DIR}")
    set(IDIFF_BUILD_NFD OFF)
endif()

# LibRaw -- try find_package (CONFIG mode for vcpkg), then pkg-config,
# then build from source.
find_package(libraw CONFIG QUIET)
if(libraw_FOUND)
    message(STATUS "Found system LibRaw via CMake config")
    set(IDIFF_BUILD_LIBRAW OFF)
    set(LIBRAW_TARGET libraw::raw_r)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw)
    endif()
    if(LIBRAW_FOUND)
        message(STATUS "Found system LibRaw via pkg-config")
        set(IDIFF_BUILD_LIBRAW OFF)
        set(LIBRAW_TARGET PkgConfig::LIBRAW)
    else()
        message(STATUS "LibRaw not found on system -- will build from source")
        set(IDIFF_BUILD_LIBRAW ON)
        set(LIBRAW_TARGET PkgConfig::LIBRAW)
    endif()
endif()

add_subdirectory(third_party)
add_subdirectory(src)

option(IDIFF_BUILD_TESTS "Build unit tests" ON)
if(IDIFF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
