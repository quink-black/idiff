add_library(idiff_util
    logger.cpp
)

target_include_directories(idiff_util PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

# Logger uses std::filesystem and a worker thread.
find_package(Threads REQUIRED)
target_link_libraries(idiff_util PUBLIC Threads::Threads)

if(WIN32)
    target_compile_definitions(idiff_util PUBLIC NOMINMAX)
    target_compile_options(idiff_util PUBLIC /utf-8)
endif()

set_target_properties(idiff_util PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
