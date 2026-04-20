add_library(idiff_core
    image.cpp
    image_loader.cpp
    image_processor.cpp
    image_comparator.cpp
    media_source.cpp
    metrics_engine.cpp
    url_cache.cpp
    comparison_config.cpp
    detail/raw_loader.cpp
)

target_include_directories(idiff_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(idiff_core PUBLIC
    opencv_core
    opencv_imgcodecs
    opencv_imgproc
    opencv_quality
    nlohmann_json::nlohmann_json
    ${LIBRAW_TARGET}
)

if(IDIFF_HAVE_IMAGEMAGICK)
    target_compile_definitions(idiff_core PUBLIC IDIFF_HAVE_MAGICK)
    target_link_libraries(idiff_core PUBLIC PkgConfig::MAGICKXX)
endif()

# Windows: NOMINMAX prevents <windows.h> from defining min/max macros
# that conflict with std::numeric_limits<T>::max() etc.  /utf-8 tells
# MSVC to treat source and execution character sets as UTF-8, which
# prevents C4819 warnings and incorrect parsing of non-ASCII comments.
if(WIN32)
    target_compile_definitions(idiff_core PUBLIC NOMINMAX)
    target_compile_options(idiff_core PUBLIC /utf-8)
endif()

set_target_properties(idiff_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
