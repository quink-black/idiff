add_library(idiff_domain
    image_library.cpp
)

target_include_directories(idiff_domain PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

# Domain depends on:
#   * idiff_core for ImageEntry's components (Image, MediaSource, ...)
#   * idiff_util for the logger
# It does NOT link SDL directly; the texture uploader interface is
# defined in src/app/io but consumed here only through the abstract
# base class, so there is no SDL link dependency.
target_link_libraries(idiff_domain PUBLIC
    idiff_core
    idiff_util
)

# We do, however, need SDL headers visible because ImageEntry currently
# stores SDL_Texture* (an opaque pointer).  Moving SDL out of the
# domain layer entirely is a future step.
find_package(SDL2 REQUIRED CONFIG)
target_link_libraries(idiff_domain PUBLIC SDL2::SDL2)

if(WIN32)
    target_compile_definitions(idiff_domain PUBLIC NOMINMAX)
    target_compile_options(idiff_domain PUBLIC /utf-8)
endif()

set_target_properties(idiff_domain PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
