cmake_minimum_required(VERSION 3.20)
project(idiff VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type defaults to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Xcode 16 omits ad-hoc code signing when no developer identity is set,
# leaving arm64 executables unsigned and killed by the kernel at launch
# (SIGKILL, no crash report). These attributes reduce signing warnings;
# the actual ad-hoc signature is applied by POST_BUILD codesign steps on
# each target (see src/app/CMakeLists.txt, tests/CMakeLists.txt).
if(XCODE AND APPLE)
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual")
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "")
    set(CMAKE_XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "NO")
endif()

# Sanitizers for memory safety debugging
# By default, sanitizers are ON for Debug builds, OFF for Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEFAULT_SANITIZERS ON)
else()
    set(DEFAULT_SANITIZERS OFF)
endif()

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" ${DEFAULT_SANITIZERS})

if(ENABLE_SANITIZERS)
    message(STATUS "Enabling sanitizers: AddressSanitizer and UndefinedBehaviorSanitizer")
    if(MSVC)
        add_compile_options(/fsanitize=address)
        # Emit debug symbols and PDB so ASAN stack traces are readable
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            add_compile_options(/Zi)
            add_link_options(/DEBUG)
        endif()
    else()
        # GCC/Clang
        add_compile_options(-fsanitize=address -fsanitize=undefined -g)
        add_link_options(-fsanitize=address -fsanitize=undefined)
    endif()
else()
    message(STATUS "Sanitizers disabled")
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
            string(REGEX MATCH "^[0-9]+" MAGICKXX_MAJOR "${MAGICKXX_VERSION}")
            message(STATUS "Found Magick++ (pkg-config): ${MAGICKXX_VERSION} (major: ${MAGICKXX_MAJOR})")

            if(MAGICKXX_MAJOR AND (MAGICKXX_MAJOR LESS 7))
                message(WARNING "Magick++ version ${MAGICKXX_VERSION} is older than the supported minimum (7). Disabling ImageMagick backend.")
                set(MAGICKXX_FOUND OFF)
            endif()
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

# FFmpeg -- optional video decoding backend.  When available, idiff can
# open container formats (MP4, MKV, MOV, etc.) as multi-frame sources.
#
# Values:
#   AUTO (default): use FFmpeg if pkg-config finds it, otherwise skip.
#   ON:             require FFmpeg; fail configuration if missing.
#   OFF:            skip FFmpeg entirely, no video file support.
set(IDIFF_WITH_FFMPEG "AUTO" CACHE STRING
    "Enable FFmpeg video decoder backend: AUTO | ON | OFF")
set_property(CACHE IDIFF_WITH_FFMPEG PROPERTY STRINGS AUTO ON OFF)

set(IDIFF_HAVE_FFMPEG OFF)
if(NOT IDIFF_WITH_FFMPEG STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        # FFmpeg 8.0 is the minimum supported upstream release.  We pin
        # every sub-library to the matching minor version so misconfigured
        # systems fail at configure time rather than at runtime with
        # cryptic symbol errors.  libavfilter is treated as a hard
        # requirement: the display path uses vf_scale for full HDR /
        # wide-gamut color management, and we deliberately keep no
        # legacy swscale-only fallback.
        #
        #   ffmpeg 8.0  ->  avformat 62.3  / avcodec 62.11 / avutil 60.8
        #                   swscale 9.1    / avfilter 11.4

        if(IDIFF_WITH_FFMPEG STREQUAL "ON")
            pkg_check_modules(FFMPEG_LIBAVFORMAT  REQUIRED IMPORTED_TARGET libavformat>=62.3)
            pkg_check_modules(FFMPEG_LIBAVCODEC   REQUIRED IMPORTED_TARGET libavcodec>=62.11)
            pkg_check_modules(FFMPEG_LIBAVUTIL    REQUIRED IMPORTED_TARGET libavutil>=60.8)
            pkg_check_modules(FFMPEG_LIBSWSCALE   REQUIRED IMPORTED_TARGET libswscale>=9.1)
            pkg_check_modules(FFMPEG_LIBAVFILTER  REQUIRED IMPORTED_TARGET libavfilter>=11.4)
        else()
            pkg_check_modules(FFMPEG_LIBAVFORMAT  QUIET IMPORTED_TARGET libavformat>=62.3)
            pkg_check_modules(FFMPEG_LIBAVCODEC   QUIET IMPORTED_TARGET libavcodec>=62.11)
            pkg_check_modules(FFMPEG_LIBAVUTIL    QUIET IMPORTED_TARGET libavutil>=60.8)
            pkg_check_modules(FFMPEG_LIBSWSCALE   QUIET IMPORTED_TARGET libswscale>=9.1)
            pkg_check_modules(FFMPEG_LIBAVFILTER  QUIET IMPORTED_TARGET libavfilter>=11.4)
        endif()

        if(FFMPEG_LIBAVFORMAT_FOUND AND FFMPEG_LIBAVCODEC_FOUND AND
           FFMPEG_LIBAVUTIL_FOUND  AND FFMPEG_LIBSWSCALE_FOUND  AND
           FFMPEG_LIBAVFILTER_FOUND)
            set(IDIFF_HAVE_FFMPEG ON)
            message(STATUS "FFmpeg found -- enabling video decoder backend "
                           "(avformat ${FFMPEG_LIBAVFORMAT_VERSION}, "
                           "avcodec ${FFMPEG_LIBAVCODEC_VERSION}, "
                           "swscale ${FFMPEG_LIBSWSCALE_VERSION}, "
                           "avfilter ${FFMPEG_LIBAVFILTER_VERSION})")
        endif()
    elseif(IDIFF_WITH_FFMPEG STREQUAL "ON")
        message(FATAL_ERROR
                "IDIFF_WITH_FFMPEG=ON but pkg-config is not available")
    endif()
endif()

if(NOT IDIFF_HAVE_FFMPEG)
    message(STATUS
        "FFmpeg video decoder backend disabled -- "
        "video container files (MP4, MKV, etc.) will not be supported")
endif()

# HEIF / AVIF decoder selection.
#
# FFmpeg ≥ 8.0 ships HEVC + AV1 decoders and the ISOBMFF demuxer that
# parses HEIF / AVIF tile-grid metadata; combined with libavfilter
# (already linked when IDIFF_HAVE_FFMPEG is ON) idiff can natively
# read .heic / .heif / .hif / .avif without pulling in libheif /
# libavif / libde265 / dav1d as direct dependencies.  ImageMagick is
# kept as a fallback for builds without FFmpeg, since it can also
# read those formats when its libheif delegate is present.
set(IDIFF_HAVE_FFMPEG_IMAGE_DECODE OFF)
if(IDIFF_HAVE_FFMPEG)
    set(IDIFF_HAVE_FFMPEG_IMAGE_DECODE ON)
endif()

if(IDIFF_HAVE_FFMPEG_IMAGE_DECODE)
    message(STATUS "HEIF / AVIF backend: FFmpeg (preferred), "
                   "ImageMagick (fallback if available)")
elseif(IDIFF_HAVE_IMAGEMAGICK)
    message(STATUS "HEIF / AVIF backend: ImageMagick "
                   "(install FFmpeg ≥ 8.0 for native support)")
else()
    message(STATUS "HEIF / AVIF backend: NONE -- .heic / .avif files "
                   "cannot be opened in this build")
endif()

# SDL2
find_package(SDL2 REQUIRED CONFIG)

# --- Dependencies with source-build fallback ---

# Catch2 v3
# When sanitizers are enabled, system Catch2 was likely built without ASAN
# and will cause annotate_vector ABI mismatch on MSVC.  Force source build.
if(ENABLE_SANITIZERS)
    message(STATUS "Sanitizers enabled -- building Catch2 from source for ABI compatibility")
    set(IDIFF_BUILD_CATCH2 ON)
else()
    find_package(Catch2 3 QUIET)
    if(NOT Catch2_FOUND)
        message(STATUS "Catch2 not found on system -- will build from source")
        set(IDIFF_BUILD_CATCH2 ON)
    else()
        message(STATUS "Found system Catch2: ${Catch2_DIR}")
        set(IDIFF_BUILD_CATCH2 OFF)
    endif()
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

# nlohmann/json (header-only) -- used by the comparison-config loader.
# Resolved at top level so the IMPORTED target
# `nlohmann_json::nlohmann_json` is visible from every subdirectory
# (find_package() inside a subdirectory only exports targets to that
# subdirectory by default, which breaks linking from src/core).
find_package(nlohmann_json 3.7 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found on system -- will build from source")
    set(IDIFF_BUILD_JSON ON)
else()
    message(STATUS "Found system nlohmann_json: ${nlohmann_json_DIR}")
    set(IDIFF_BUILD_JSON OFF)
endif()

# Asio (standalone, header-only) -- transport library for the in-process
# JSON-RPC server (idiff_rpc).  Used to drive the Unix Domain Socket
# acceptor (POSIX) or Windows Named Pipe accept thread on a private I/O
# thread.  Resolved at top level (mirroring the nlohmann_json pattern) so
# the IMPORTED `asio::asio` target is visible from every subdirectory.
#
# Detection order:
#   1. find_path(asio.hpp) in common system / Homebrew / vcpkg locations.
#   2. Fall back to FetchContent (third_party/CMakeLists.txt) when absent.
#
# IDIFF_HAVE_RPC is defined for the rest of the build to gate the
# idiff_rpc subdirectory and the executable's RPC integration.
if(WIN32)
    # Windows: Asio is unlikely on the system; always fetch from source.
    set(IDIFF_BUILD_ASIO ON)
else()
    find_path(ASIO_INCLUDE_DIR asio.hpp
        HINTS
            /opt/homebrew/include
            /usr/local/include
            /usr/include
        DOC "Path to standalone Asio headers (asio.hpp)")
    if(ASIO_INCLUDE_DIR)
        message(STATUS "Found system Asio: ${ASIO_INCLUDE_DIR}")
        add_library(asio_asio INTERFACE)
        target_include_directories(asio_asio INTERFACE "${ASIO_INCLUDE_DIR}")
        target_compile_definitions(asio_asio INTERFACE ASIO_STANDALONE)
        add_library(asio::asio ALIAS asio_asio)
        set(IDIFF_BUILD_ASIO OFF)
    else()
        message(STATUS "Asio not found on system -- will fetch from source")
        set(IDIFF_BUILD_ASIO ON)
    endif()
endif()
set(IDIFF_HAVE_RPC ON)

# LibRaw -- optional, controlled by IDIFF_WITH_LIBRAW.  Detected via
# find_package (CONFIG mode for vcpkg) first, then pkg-config.  Source
# builds were dropped because LibRaw has its own non-trivial dependencies
# (LCMS2, jpeg, jasper) and tickles OpenMP / clang quirks on macOS;
# users who need RAW support should install it from their package
# manager (brew install libraw / apt install libraw-dev / vcpkg).
#
# Values:
#   AUTO (default): use LibRaw if found on the system, otherwise skip.
#   ON:             require LibRaw; fail configuration if missing.
#   OFF:            skip LibRaw entirely, no RAW file support.
set(IDIFF_WITH_LIBRAW "AUTO" CACHE STRING
    "Enable LibRaw RAW decoder backend: AUTO | ON | OFF")
set_property(CACHE IDIFF_WITH_LIBRAW PROPERTY STRINGS AUTO ON OFF)

set(IDIFF_HAVE_LIBRAW OFF)
set(LIBRAW_TARGET "")
if(NOT IDIFF_WITH_LIBRAW STREQUAL "OFF")
    find_package(libraw CONFIG QUIET)
    if(libraw_FOUND)
        message(STATUS "Found system LibRaw via CMake config")
        set(IDIFF_HAVE_LIBRAW ON)
        set(LIBRAW_TARGET libraw::raw_r)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw)
        endif()
        if(LIBRAW_FOUND)
            message(STATUS "Found system LibRaw via pkg-config")
            set(IDIFF_HAVE_LIBRAW ON)
            set(LIBRAW_TARGET PkgConfig::LIBRAW)
        elseif(IDIFF_WITH_LIBRAW STREQUAL "ON")
            message(FATAL_ERROR
                    "IDIFF_WITH_LIBRAW=ON but LibRaw was not found "
                    "(install libraw via brew / apt / vcpkg)")
        endif()
    endif()
endif()

if(NOT IDIFF_HAVE_LIBRAW)
    message(STATUS
        "LibRaw RAW decoder disabled -- "
        "camera RAW files (.dng, .cr2, .nef, ...) will not be supported")
endif()

add_subdirectory(third_party)
add_subdirectory(src)

option(IDIFF_BUILD_TESTS "Build unit tests" ON)
if(IDIFF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
