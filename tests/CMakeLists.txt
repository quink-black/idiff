add_executable(idiff_tests
    test_image.cpp
    test_image_loader.cpp
    test_image_processor.cpp
    test_image_comparator.cpp
    test_media_source.cpp
    test_metrics_engine.cpp
    test_ruler_utils.cpp
    test_yuv_source.cpp
    test_sr_engine.cpp
    test_channel_view.cpp
    # SR engine sources needed by test_sr_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/app/sr_infer_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/app/sr_infer_engine_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/app/seedvr2_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/app/sr_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/app/settings.cpp
)

# sr_dialog.cpp uses ImGui — provide a stub-free include path.
# The test only calls sr_default_output_path() which does not
# touch ImGui, but the translation unit still needs the header.
target_include_directories(idiff_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(idiff_tests PRIVATE
    idiff_core
    imgui
    Catch2::Catch2WithMain
)

if(WIN32)
    target_compile_definitions(idiff_tests PRIVATE NOMINMAX)
    target_compile_options(idiff_tests PRIVATE /utf-8)
endif()

include(CTest)
include(Catch)
catch_discover_tests(idiff_tests)
