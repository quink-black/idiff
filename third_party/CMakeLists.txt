# Third-party dependencies.
# ImGui is always built from source (no standard system package).
# Catch2, nfd, and LibRaw are built from source only when not found on the system
# (controlled by IDIFF_BUILD_* flags set in the root CMakeLists.txt).

include(FetchContent)

# ---- Dear ImGui (always from source) ----

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(imgui)

# Build ImGui with SDL2 + SDL_Renderer backends
set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL2::SDL2)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

# ---- Catch2 v3 (from source when not found on system) ----

if(IDIFF_BUILD_CATCH2)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    # Catch2's CMake build provides Catch2::Catch2WithMain automatically.
endif()

# ---- Native File Dialog Extended (from source when not found on system) ----

if(IDIFF_BUILD_NFD)
    FetchContent_Declare(
        nativefiledialog-extended
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nativefiledialog-extended)
    # nfd's CMakeLists.txt creates the nfd::nfd target automatically.
endif()

# ---- LibRaw (from source when not found on system) ----

if(IDIFF_BUILD_LIBRAW)
    FetchContent_Declare(
        libraw
        GIT_REPOSITORY https://github.com/LibRaw/LibRaw.git
        GIT_TAG        0.21.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(libraw)

    # LibRaw's CMake build creates a 'raw' target.  Our code links against
    # PkgConfig::LIBRAW when using the system package.  Create an ALIAS so
    # both paths can use the same target name.
    if(TARGET raw AND NOT TARGET PkgConfig::LIBRAW)
        add_library(PkgConfig::LIBRAW ALIAS raw)
    endif()
endif()
