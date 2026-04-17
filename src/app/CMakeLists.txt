add_executable(idiff
    main.cpp
    app.cpp
    viewport.cpp
    metrics_panel.cpp
    properties_panel.cpp
    settings.cpp
)

if(APPLE)
    target_sources(idiff PRIVATE platform/platform_macos.mm)
elseif(WIN32)
    target_sources(idiff PRIVATE platform/platform_win32.cpp)
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
endif()
