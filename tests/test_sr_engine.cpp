#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "app/sr_dialog.h"
#include "app/sr_infer_engine.h"
#include "app/sr_infer_engine_factory.h"
#include "app/seedvr2_engine.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace fs = std::filesystem;

// ─── sr_default_output_path ───────────────────────────────────────────

TEST_CASE("sr_default_output_path generates correct names", "[sr]") {
    SECTION("2x scale with .png") {
        auto out = idiff::sr_default_output_path("C:/photos/cat.png", 2);
        REQUIRE(out.filename().string() == "cat_sr_2x.png");
        REQUIRE(out.parent_path().string() == "C:/photos");
    }
    SECTION("4x scale with .jpg") {
        auto out = idiff::sr_default_output_path("/home/user/photo.jpg", 4);
        REQUIRE(out.filename().string() == "photo_sr_4x.jpg");
    }
    SECTION("no extension defaults to .png") {
        auto out = idiff::sr_default_output_path("image", 2);
        REQUIRE(out.filename().string() == "image_sr_2x.png");
    }
    SECTION("preserves directory path") {
        auto out = idiff::sr_default_output_path("D:/work/test.bmp", 2);
        REQUIRE(out.parent_path().string() == "D:/work");
        REQUIRE(out.filename().string() == "test_sr_2x.bmp");
    }
}

// ─── SRInferEngineFactory ─────────────────────────────────────────────

TEST_CASE("SRInferEngineFactory registration and creation", "[sr]") {
    auto& factory = idiff::SRInferEngineFactory::instance();

    SECTION("unregistered engine returns nullptr") {
        auto engine = factory.create_engine("nonexistent_engine_xyz");
        REQUIRE(engine == nullptr);
    }

    SECTION("has_engine returns false for unregistered") {
        REQUIRE_FALSE(factory.has_engine("nonexistent_engine_xyz"));
    }
}

// ─── SeedVR2Engine error handling ─────────────────────────────────────

TEST_CASE("SeedVR2Engine rejects non-existent input file", "[sr]") {
    // Use a dummy upscaler path — we won't get far enough to need it
    // because the input file check happens first.
    // But we need python.exe and upscale.py to exist for the pre-checks.
    // So use a path that doesn't have them — the engine should fail
    // at the python_exe check, not the input check.
    // Actually, the input file check happens before the python check.
    idiff::SeedVR2Engine engine(fs::temp_directory_path());

    bool started = engine.start_inference(
        "C:/nonexistent/path/to/image.png",
        "C:/nonexistent/output.png",
        2, 256, 64);

    REQUIRE_FALSE(started);
    REQUIRE(engine.get_status() == idiff::SREngineStatus::Failed);
    auto err = engine.last_error();
    REQUIRE(err.type == "io");
    REQUIRE(err.description.find("does not exist") != std::string::npos);
}

TEST_CASE("SeedVR2Engine rejects missing python interpreter", "[sr]") {
    // Create a temp dir with no python/ subdirectory
    auto tmp = fs::temp_directory_path() / "idiff_test_sr_no_python";
    fs::create_directories(tmp);

    // Create a dummy input file
    auto input = tmp / "input.png";
    { std::ofstream f(input, std::ios::binary); f << "dummy"; }

    idiff::SeedVR2Engine engine(tmp);

    bool started = engine.start_inference(input, tmp / "output.png", 2, 256, 64);

    REQUIRE_FALSE(started);
    REQUIRE(engine.get_status() == idiff::SREngineStatus::Failed);
    auto err = engine.last_error();
    REQUIRE(err.type == "io");
    REQUIRE(err.description.find("Python interpreter not found") != std::string::npos);

    // Cleanup
    fs::remove_all(tmp);
}

TEST_CASE("SeedVR2Engine rejects missing upscale script", "[sr]") {
    // Create a temp dir with python/python.exe but no app/upscale.py
    auto tmp = fs::temp_directory_path() / "idiff_test_sr_no_script";
    fs::create_directories(tmp / "python");

    // Create a dummy python.exe
    auto python_exe = tmp / "python" / "python.exe";
    { std::ofstream f(python_exe, std::ios::binary); f << "dummy"; }

    // Create a dummy input file
    auto input = tmp / "input.png";
    { std::ofstream f(input, std::ios::binary); f << "dummy"; }

    idiff::SeedVR2Engine engine(tmp);

    bool started = engine.start_inference(input, tmp / "output.png", 2, 256, 64);

    REQUIRE_FALSE(started);
    REQUIRE(engine.get_status() == idiff::SREngineStatus::Failed);
    auto err = engine.last_error();
    REQUIRE(err.type == "io");
    REQUIRE(err.description.find("Upscale script not found") != std::string::npos);

    // Cleanup
    fs::remove_all(tmp);
}

// ─── SeedVR2Engine::parse_progress ────────────────────────────────────

TEST_CASE("SeedVR2Engine::parse_progress parses various formats", "[sr]") {
    SECTION("explicit percentage: Progress: 42.5%") {
        float p = idiff::SeedVR2Engine::parse_progress("Progress: 42.5%");
        REQUIRE(p == Catch::Approx(0.425f).margin(0.001f));
    }
    SECTION("explicit percentage: Progress: 100%") {
        float p = idiff::SeedVR2Engine::parse_progress("Progress: 100%");
        REQUIRE(p == Catch::Approx(1.0f).margin(0.001f));
    }
    SECTION("explicit percentage: Progress: 0%") {
        float p = idiff::SeedVR2Engine::parse_progress("Progress: 0%");
        REQUIRE(p == Catch::Approx(0.0f).margin(0.001f));
    }
    SECTION("fractional progress: progress:0.425") {
        float p = idiff::SeedVR2Engine::parse_progress("phase:upscale progress:0.425");
        REQUIRE(p == Catch::Approx(0.425f).margin(0.001f));
    }
    SECTION("fractional progress: progress:1.0") {
        float p = idiff::SeedVR2Engine::parse_progress("progress:1.0");
        REQUIRE(p == Catch::Approx(1.0f).margin(0.001f));
    }
    SECTION("tile progress: Processing tile 1/4") {
        float p = idiff::SeedVR2Engine::parse_progress(
            "[12:34:56.789] \xF0\x9F\x8E\xAC Processing tile 1/4 [256x256]");
        REQUIRE(p == Catch::Approx(0.25f).margin(0.001f));
    }
    SECTION("tile progress: Processing tile 3/4") {
        float p = idiff::SeedVR2Engine::parse_progress(
            "Processing tile 3/4 [512x512]");
        REQUIRE(p == Catch::Approx(0.75f).margin(0.001f));
    }
    SECTION("tile progress: Processing tile 4/4") {
        float p = idiff::SeedVR2Engine::parse_progress(
            "Processing tile 4/4 [256x256]");
        REQUIRE(p == Catch::Approx(1.0f).margin(0.001f));
    }
    SECTION("no progress info returns -1") {
        float p = idiff::SeedVR2Engine::parse_progress(
            "Loading model seedvr2_ema_3b-Q4_K_M.gguf...");
        REQUIRE(p == Catch::Approx(-1.0f));
    }
    SECTION("empty string returns -1") {
        float p = idiff::SeedVR2Engine::parse_progress("");
        REQUIRE(p == Catch::Approx(-1.0f));
    }
}
// This test only runs when SEEDVR2_UPSCALER_PATH is set and points to
// a valid seedvr2-upscaler installation.  It is skipped in CI.

TEST_CASE("SeedVR2Engine end-to-end upscale", "[sr][integration]") {
    // Try to find the upscaler
    auto upscaler_path = idiff::SeedVR2Engine::resolve_upscaler_path();
    if (upscaler_path.empty()) {
        WARN("seedvr2-upscaler not found; set SEEDVR2_UPSCALER_PATH to enable");
        return;
    }

    auto python_exe = upscaler_path / "python" / "python.exe";
    auto upscale_script = upscaler_path / "app" / "upscale.py";
    if (!fs::exists(python_exe) || !fs::exists(upscale_script)) {
        WARN("seedvr2-upscaler directory incomplete");
        return;
    }

    // Create a small test image using OpenCV
    auto tmp = fs::temp_directory_path() / "idiff_test_sr_e2e";
    fs::create_directories(tmp);
    auto input_path = tmp / "test_input.png";
    auto output_path = tmp / "test_sr_2x.png";

    // Generate a 64x64 solid gray test image
    {
        cv::Mat img(64, 64, CV_8UC3, cv::Scalar(128, 128, 128));
        bool ok = cv::imwrite(input_path.string(), img);
        REQUIRE(ok);
    }

    REQUIRE(fs::exists(input_path));

    idiff::SeedVR2Engine engine(upscaler_path);

    bool started = engine.start_inference(
        input_path, output_path,
        2,    // scale
        256,  // tile_size
        64,   // tile_overlap
        "seedvr2_ema_3b-Q4_K_M.gguf",
        "lab");

    REQUIRE(started);
    REQUIRE(engine.get_status() == idiff::SREngineStatus::Running);

    // Poll until completion or timeout (5 minutes for GPU inference)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (engine.get_status() == idiff::SREngineStatus::Running) {
        if (std::chrono::steady_clock::now() > deadline) {
            engine.cancel();
            FAIL("Timed out waiting for super resolution to complete");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto status = engine.get_status();
    if (status == idiff::SREngineStatus::Failed) {
        auto err = engine.last_error();
        FAIL("Super resolution failed: " + err.description);
    }

    REQUIRE(status == idiff::SREngineStatus::Completed);
    REQUIRE(engine.get_output_path() == output_path);
    REQUIRE(fs::exists(output_path));

    // Verify the output image is larger than the input
    auto input_size = fs::file_size(input_path);
    auto output_size = fs::file_size(output_path);
    REQUIRE(output_size > input_size);

    // Cleanup
    fs::remove_all(tmp);
}
