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
    test_depth_utils.cpp
    test_file_watcher.cpp
    test_logger.cpp
    test_io_interfaces.cpp
    test_image_library.cpp
    test_selection_model.cpp
    test_timeline_model.cpp
    test_diff_service.cpp
    test_sr_task_service.cpp
    test_comparison_config_service.cpp
    test_app_controller.cpp
    test_controller_integration.cpp
    test_pixel_sampler.cpp
    # test_logging_integration.cpp  # FIXME: linking issue with ImageLibrary::upload
    # SR engine sources needed by test_sr_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/app/sr_infer_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/app/sr_infer_engine_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/app/seedvr2_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/app/sr_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/app/settings.cpp
    # Pixel sampler is exercised by test_pixel_sampler.cpp
    ${CMAKE_SOURCE_DIR}/src/app/pixel_sampler.cpp
    # IO adapters under test
    ${CMAKE_SOURCE_DIR}/src/app/io/texture_uploader.cpp
    ${CMAKE_SOURCE_DIR}/src/app/io/file_dialog.cpp
    # Controller is part of app/, not idiff_domain — pull it in for
    # test_app_controller.cpp without dragging in the full app.cpp.
    ${CMAKE_SOURCE_DIR}/src/app/controller.cpp
)

# Conditionally add video decoder tests when FFmpeg is available
if(IDIFF_HAVE_FFMPEG)
    target_sources(idiff_tests PRIVATE
        test_video_decoder.cpp
        test_video_filter_graph.cpp
    )
endif()

# sr_dialog.cpp uses ImGui — provide a stub-free include path.
# The test only calls sr_default_output_path() which does not
# touch ImGui, but the translation unit still needs the header.
target_include_directories(idiff_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(idiff_tests PRIVATE
    idiff_domain
    imgui
    SDL2::SDL2
    nfd::nfd
    Catch2::Catch2WithMain
)

if(WIN32)
    target_compile_definitions(idiff_tests PRIVATE NOMINMAX)
    target_compile_options(idiff_tests PRIVATE /utf-8)
endif()

include(CTest)
include(Catch)

# When sanitizers are enabled, pass an LSan suppression file so that known
# third-party leaks (e.g. GPU driver singletons from OpenCL) do not cause
# test failures.
set(_test_properties SKIP_RETURN_CODE 4)
if(ENABLE_SANITIZERS)
    set(_lsan_supp "${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")
    list(APPEND _test_properties
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${_lsan_supp}")
endif()

catch_discover_tests(idiff_tests
    PROPERTIES ${_test_properties}
)

# --- Test video generation (FFmpeg) ---
# Generate a short test video (10 frames, 64x64, each frame a different solid
# color) for the video decoder unit tests.  The video is placed in the build
# directory and its path is passed as a compile definition.
if(IDIFF_HAVE_FFMPEG)
    set(TEST_VIDEO_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_data")
    set(TEST_VIDEO_PATH "${TEST_VIDEO_DIR}/test_10frames_64x64.mp4")

    find_program(FFMPEG_EXECUTABLE ffmpeg)
    if(FFMPEG_EXECUTABLE)
        add_custom_command(
            OUTPUT "${TEST_VIDEO_PATH}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TEST_VIDEO_DIR}"
            COMMAND ${FFMPEG_EXECUTABLE}
                -y -f lavfi
                -i "color=c=red:size=64x64:rate=10:duration=1,format=yuv420p"
                -c:v libx264 -preset ultrafast -qp 0
                -frames:v 10
                "${TEST_VIDEO_PATH}"
            COMMENT "Generating test video: ${TEST_VIDEO_PATH}"
            VERBATIM
        )
        add_custom_target(generate_test_video DEPENDS "${TEST_VIDEO_PATH}")
        add_dependencies(idiff_tests generate_test_video)

        target_compile_definitions(idiff_tests PRIVATE
            IDIFF_TEST_VIDEO_PATH="${TEST_VIDEO_PATH}"
        )
    else()
        message(WARNING "ffmpeg CLI not found -- video decoder tests will be skipped at runtime")
        target_compile_definitions(idiff_tests PRIVATE
            IDIFF_TEST_VIDEO_PATH=""
        )
    endif()
endif()
