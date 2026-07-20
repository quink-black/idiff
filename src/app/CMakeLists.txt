add_executable(idiff
    main.cpp
    app.cpp
    idle_policy.cpp
    controller.cpp
    viewport.cpp
    metrics_panel.cpp
    pixel_sampler.cpp
    pixel_inspector_panel.cpp
    properties_panel.cpp
    settings.cpp
    sr_infer_engine.cpp
    sr_infer_engine_factory.cpp
    seedvr2_engine.cpp
    sr_dialog.cpp
    screenshot_composer.cpp
    io/texture_uploader.cpp
    io/file_dialog.cpp
    ui/dialogs.cpp
    ui/image_list.cpp
    ui/inspector.cpp
    ui/status_bar.cpp
    ui/toolbar.cpp
    ui/viewport_panel.cpp
    ui/yuv_dialog.cpp
)

# JSON-RPC server.  IDIFF_HAVE_RPC is set in the top-level CMakeLists.txt.
if(IDIFF_HAVE_RPC)
    add_subdirectory(rpc)
    target_sources(idiff PRIVATE app_rpc_methods.cpp)
endif()

if(APPLE)
    target_sources(idiff PRIVATE platform/platform_macos.mm)
    set(IDIFF_ICON "${CMAKE_SOURCE_DIR}/resource/idiff.icns")
    target_sources(idiff PRIVATE "${IDIFF_ICON}")
    set_source_files_properties("${IDIFF_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
elseif(WIN32)
    target_sources(idiff PRIVATE platform/platform_win32.cpp)
    target_sources(idiff PRIVATE platform/idiff.rc)
    # Listed as a source for IDE visibility only; the linker picks it
    # up via the /MANIFESTINPUT option set further down.
    target_sources(idiff PRIVATE platform/idiff.manifest)
    set_source_files_properties(platform/idiff.manifest
        PROPERTIES HEADER_FILE_ONLY TRUE)
    # Hand the manifest to mt.exe via the linker so it gets merged
    # with MSVC's auto-generated CRT dependency manifest.  Embedding
    # the file directly through `RT_MANIFEST` in idiff.rc instead
    # produces a malformed RT_MANIFEST resource on some Windows SDK
    # versions (an 11-byte raw-resource prefix gets prepended to the
    # XML), which SxS then rejects with error 14001 at process
    # launch.  /MANIFESTINPUT routes through mt.exe's manifest tool,
    # which knows how to author RT_MANIFEST correctly and how to
    # combine multiple manifest fragments.
    if(MSVC)
        target_link_options(idiff PRIVATE
            "/MANIFEST"
            "/MANIFEST:EMBED"
            "/MANIFESTUAC:NO"
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/platform/idiff.manifest")
    endif()
else()
    target_sources(idiff PRIVATE platform/platform_posix.cpp)
endif()

target_link_libraries(idiff PRIVATE
    idiff_domain
    imgui
    SDL2::SDL2
    SDL2::SDL2main
    nfd::nfd
)

if(IDIFF_HAVE_RPC)
    target_link_libraries(idiff PRIVATE idiff_rpc)
    target_compile_definitions(idiff PRIVATE IDIFF_HAVE_RPC=1)
endif()

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
        MACOSX_BUNDLE_ICON_FILE idiff.icns
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

# Copy idiff.png next to the executable so set_window_icon() can find it
# at runtime.  On macOS the png is also placed in Contents/Resources so
# the bundle is self-contained.
if(APPLE)
    set(IDIFF_ICON_PNG "${CMAKE_SOURCE_DIR}/resource/idiff.png")
    target_sources(idiff PRIVATE "${IDIFF_ICON_PNG}")
    set_source_files_properties("${IDIFF_ICON_PNG}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
else()
    add_custom_command(TARGET idiff POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/resource/idiff.png"
                "$<TARGET_FILE_DIR:idiff>/idiff.png"
        VERBATIM
        COMMENT "Copying idiff.png next to executable"
    )
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
