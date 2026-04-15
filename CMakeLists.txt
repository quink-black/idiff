cmake_minimum_required(VERSION 3.20)
project(idiff VERSION 0.1.0 LANGUAGES CXX OBJCXX)

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

# OpenCV
find_package(OpenCV REQUIRED COMPONENTS core imgproc quality)

# ImageMagick (pkg-config)
pkg_check_modules(MAGICKXX REQUIRED IMPORTED_TARGET Magick++)

# LibRaw (pkg-config)
pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw)

# SDL2
find_package(SDL2 REQUIRED)

# Catch2 v3
find_package(Catch2 3 REQUIRED)

# Native File Dialog Extended
find_package(nfd REQUIRED)

add_subdirectory(third_party)
add_subdirectory(src)

option(IDIFF_BUILD_TESTS "Build unit tests" ON)
if(IDIFF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
