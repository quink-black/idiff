# Third-party dependencies.
# ImGui is always built from source (no standard system package).
# Catch2 and nfd are built from source only when not found on the system
# (controlled by IDIFF_BUILD_* flags set in the root CMakeLists.txt).
# LibRaw is detected only on the system; users who want RAW support
# install it via brew / apt / vcpkg.

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

# ---- nlohmann/json (header-only; from system if available, else fetch) ----
#
# Used by idiff_core for the comparison-config loader.  The find_package()
# call lives in the top-level CMakeLists.txt so the resulting IMPORTED
# target is visible from every subdirectory; here we only need to pull
# the source in when the system package was absent.
if(IDIFF_BUILD_JSON)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    OFF CACHE INTERNAL "")
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

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
    # When Catch2 is found via find_package, CMake adds its cmake/ dir to
    # CMAKE_MODULE_PATH so include(Catch) works.  With FetchContent that
    # doesn't happen, so do it manually here -- propagate to the parent
    # scope (root CMakeLists) so tests/CMakeLists.txt can include(Catch).
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
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

# ---- Asio (standalone, header-only) ----
#
# Used by idiff_rpc.  Header-only; we only need the include directory and
# the ASIO_STANDALONE compile definition (so Asio doesn't try to pull in
# Boost).  We pin a tagged release rather than tracking master to keep
# builds reproducible.
if(IDIFF_BUILD_ASIO)
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(asio)
    add_library(asio_asio INTERFACE)
    target_include_directories(asio_asio INTERFACE
        "${asio_SOURCE_DIR}/asio/include")
    target_compile_definitions(asio_asio INTERFACE ASIO_STANDALONE)
    add_library(asio::asio ALIAS asio_asio)
endif()
