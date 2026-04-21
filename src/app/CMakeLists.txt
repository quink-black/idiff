add_executable(idiff
    main.cpp
    app.cpp
    viewport.cpp
    metrics_panel.cpp
    properties_panel.cpp
    settings.cpp
    sr_infer_engine.cpp
    sr_infer_engine_factory.cpp
    seedvr2_engine.cpp
    sr_dialog.cpp
)

if(APPLE)
    target_sources(idiff PRIVATE platform/platform_macos.mm)
elseif(WIN32)
    target_sources(idiff PRIVATE platform/platform_win32.cpp)
else()
    target_sources(idiff PRIVATE platform/platform_posix.cpp)
endif()

target_link_libraries(idiff PRIVATE
    idiff_core
    imgui
    SDL2::SDL2
    SDL2::SDL2main
    nfd::nfd
)

# macOS frameworks (needed by platform_macos.mm)
if(APPLE)
    find_library(APPKIT_LIBRARY AppKit REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    target_link_libraries(idiff PRIVATE ${APPKIT_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# macOS app bundle info
if(APPLE)
    set_target_properties(idiff PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER com.idiff.app
        MACOSX_BUNDLE_BUNDLE_NAME idiff
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    )
endif()

# Windows: use WIN32 subsystem to suppress the console window
if(WIN32)
    set_target_properties(idiff PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
    target_compile_definitions(idiff PRIVATE NOMINMAX)
    target_compile_options(idiff PRIVATE /utf-8)
endif()

# -----------------------------------------------------------------------------
# Packaging target: `idiff_package`
#
# Produces a self-contained, distributable build artifact that does not
# depend on the developer's Homebrew / vcpkg / apt prefix:
#
#   * macOS:   runs scripts/macos_bundle_deps.sh against idiff.app to copy
#              every non-system dylib into Contents/Frameworks and rewrite
#              all load paths to @rpath/.
#   * Windows: copies every runtime DLL (from TARGET_RUNTIME_DLLS,
#              resolved by CMake against the vcpkg / system search paths)
#              next to idiff.exe.
#   * Linux:   currently a no-op -- Linux distribution is left to the
#              integrator (AppImage, deb, flatpak, etc.).
# -----------------------------------------------------------------------------

if(APPLE)
    add_custom_target(idiff_package
        COMMAND "${CMAKE_COMMAND}" -E echo
                "Bundling dependencies into idiff.app..."
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/macos_bundle_deps.sh"
                "$<TARGET_BUNDLE_DIR:idiff>"
        DEPENDS idiff
        VERBATIM
        USES_TERMINAL
        COMMENT "Packaging idiff.app as a self-contained bundle"
    )
elseif(WIN32)
    # $<TARGET_RUNTIME_DLLS:idiff> expands to the full list of DLLs CMake
    # resolved for idiff at link time -- SDL2.dll, opencv_worldXYZ.dll,
    # the ImageMagick DLLs (if present), libraw.dll, etc.
    add_custom_target(idiff_package
        COMMAND "${CMAKE_COMMAND}" -E echo
                "Copying runtime DLLs next to idiff.exe..."
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_RUNTIME_DLLS:idiff>"
                "$<TARGET_FILE_DIR:idiff>"
        COMMAND_EXPAND_LISTS
        DEPENDS idiff
        VERBATIM
        USES_TERMINAL
        COMMENT "Packaging idiff.exe with its runtime DLLs"
    )
else()
    add_custom_target(idiff_package
        COMMAND "${CMAKE_COMMAND}" -E echo
                "idiff_package is a no-op on this platform"
        DEPENDS idiff
        COMMENT "Packaging idiff (no-op on this platform)"
    )
endif()
