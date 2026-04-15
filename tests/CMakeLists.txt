add_executable(idiff_tests
    test_image.cpp
    test_image_loader.cpp
    test_image_processor.cpp
    test_image_comparator.cpp
    test_metrics_engine.cpp
)

target_link_libraries(idiff_tests PRIVATE
    idiff_core
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(idiff_tests)
