add_library(idiff_core
    image.cpp
    image_loader.cpp
    image_processor.cpp
    image_comparator.cpp
    metrics_engine.cpp
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
    ${LIBRAW_TARGET}
)

if(IDIFF_HAVE_IMAGEMAGICK)
    target_compile_definitions(idiff_core PUBLIC IDIFF_HAVE_MAGICK)
    target_link_libraries(idiff_core PUBLIC PkgConfig::MAGICKXX)
endif()

set_target_properties(idiff_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
