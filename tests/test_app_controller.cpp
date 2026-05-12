// Smoke tests for AppController orchestration helpers.
//
// AppController now owns every domain service and exposes a handful of
// composite operations that used to live inline on App (and were
// therefore unreachable from any test that did not bring up SDL +
// ImGui).  These tests exercise the cross-service contracts that App
// previously relied on:
//
//   * remove_entry triggers library removal, selection remap, swap_ab
//     reset, label recomputation and a diff dirty mark in one shot.
//   * sort_entries_by_name and move_entry both keep the selection in
//     sync with the new positions.
//   * compute_display_labels disambiguates duplicate filenames using
//     parent directories.
//   * timeline_length, has_running_sr_tasks and get_ab_indices forward
//     to the underlying service without modification.

#include <catch2/catch_test_macros.hpp>

#include "app/controller.h"
#include "app/io/texture_uploader.h"
#include "app/sr_dialog.h"
#include "app/status_reporter.h"
#include "app/app.h"
#include "domain/comparison_config_service.h"
#include "domain/diff_service.h"
#include "domain/image_library.h"
#include "domain/selection_model.h"
#include "core/image.h"        // IWYU pragma: keep
#include "core/image_loader.h"
#include "core/media_source.h" // IWYU pragma: keep
#include "core/url_cache.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Minimal ITextureUploader: the controller helpers only need add/move/
// remove/sort to work, none of which call upload().  We still provide
// a real upload() in case future tests want to assert upload counts.
class CountingUploader : public idiff::ITextureUploader {
public:
    SDL_Texture* upload(const idiff::UploadRequest&) override {
        return reinterpret_cast<SDL_Texture*>(
            static_cast<std::uintptr_t>(++next_cookie_));
    }
    void destroy(SDL_Texture*) override { ++destroy_count_; }

    int destroy_count() const { return destroy_count_; }

private:
    std::uintptr_t next_cookie_ = 0;
    int destroy_count_ = 0;
};

// Records every call so tests can assert what reached the UI.
class RecordingStatusReporter : public idiff::IStatusReporter {
public:
    void set_status(const std::string& text) override {
        status_calls.push_back(text);
        current_status = text;
    }
    void append_status(const std::string& text) override {
        append_calls.push_back(text);
        if (text.empty()) return;
        if (!current_status.empty()) current_status += " | ";
        current_status += text;
    }
    void set_sr_status(const std::string& text) override {
        sr_status_calls.push_back(text);
    }
    void show_error(const std::string& title,
                    const std::string& message) override {
        error_titles.push_back(title);
        error_messages.push_back(message);
    }

    std::vector<std::string> status_calls;
    std::vector<std::string> append_calls;
    std::vector<std::string> sr_status_calls;
    std::vector<std::string> error_titles;
    std::vector<std::string> error_messages;
    std::string current_status;
};

idiff::ImageEntry make_entry(const std::string& path,
                             const std::string& filename) {
    idiff::ImageEntry e;
    e.path = path;
    e.filename = filename;
    e.display_label = filename;
    return e;
}

// Drop a tiny solid-color PNG into a unique temp directory so
// ImageFileSource (created inside controller.load_images) can decode
// it.  Returns the absolute path; the caller owns cleanup.
std::string write_tmp_png(const std::string& tag,
                          const std::string& filename,
                          unsigned char fill = 128) {
    auto dir = std::filesystem::temp_directory_path()
             / ("idiff_ctrl_" + tag);
    std::filesystem::create_directories(dir);
    auto path = dir / filename;

    cv::Mat m(4, 4, CV_8UC3, cv::Scalar(fill, fill, fill));
    std::vector<std::uint8_t> buf;
    REQUIRE(cv::imencode(".png", m, buf));

    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    out.close();
    return path.string();
}

} // namespace

TEST_CASE("AppController::remove_entry coordinates library, selection, diff",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/x.png", "x.png"));
    controller.library().add(make_entry("/b/y.png", "y.png"));
    controller.library().add(make_entry("/c/z.png", "z.png"));

    controller.selection().insert(0);
    controller.selection().insert(2);
    controller.selection().set_swap_ab(true);

    controller.remove_entry(0);

    // x.png is gone; y.png and z.png remain at indices 0 and 1.
    REQUIRE(controller.library().all().size() == 2);
    REQUIRE(controller.library().all()[0].filename == "y.png");
    REQUIRE(controller.library().all()[1].filename == "z.png");

    // The previously-selected indices were {0, 2}; 0 dropped, 2 -> 1.
    REQUIRE(controller.selection().indices().size() == 1);
    REQUIRE(controller.selection().indices().count(1) == 1);

    // Membership changed -> swap_ab must reset.
    REQUIRE_FALSE(controller.selection().swap_ab());

    // Diff cache is dirty so the next render recomputes it.
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::remove_entry ignores out-of-range indices",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/x.png", "x.png"));

    controller.remove_entry(-1);
    controller.remove_entry(99);

    REQUIRE(controller.library().all().size() == 1);
    // The library is unchanged so no texture destruction was triggered.
    REQUIRE(uploader.destroy_count() == 0);
}

TEST_CASE("AppController::sort_entries_by_name remaps the selection",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // Reverse alphabetical order so the sort actually moves entries.
    controller.library().add(make_entry("/p/c.png", "c.png"));
    controller.library().add(make_entry("/p/b.png", "b.png"));
    controller.library().add(make_entry("/p/a.png", "a.png"));

    controller.selection().insert(0); // c.png
    controller.selection().insert(2); // a.png

    controller.sort_entries_by_name();

    REQUIRE(controller.library().all()[0].filename == "a.png");
    REQUIRE(controller.library().all()[1].filename == "b.png");
    REQUIRE(controller.library().all()[2].filename == "c.png");

    // c.png moved 0 -> 2, a.png moved 2 -> 0.  Selection follows.
    REQUIRE(controller.selection().indices() == std::set<int>{0, 2});
}

TEST_CASE("AppController::move_entry rejects no-op and bad indices",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/p/a.png", "a.png"));
    controller.library().add(make_entry("/p/b.png", "b.png"));
    controller.library().add(make_entry("/p/c.png", "c.png"));
    controller.selection().insert(0);

    controller.move_entry(1, 1);   // no-op
    controller.move_entry(-1, 0);  // reject
    controller.move_entry(0, 99);  // reject

    REQUIRE(controller.library().all()[0].filename == "a.png");
    REQUIRE(controller.library().all()[1].filename == "b.png");
    REQUIRE(controller.library().all()[2].filename == "c.png");
    REQUIRE(controller.selection().indices() == std::set<int>{0});

    controller.move_entry(0, 2);

    REQUIRE(controller.library().all()[0].filename == "b.png");
    REQUIRE(controller.library().all()[1].filename == "c.png");
    REQUIRE(controller.library().all()[2].filename == "a.png");
    REQUIRE(controller.selection().indices() == std::set<int>{2});
}

TEST_CASE("AppController::compute_display_labels disambiguates duplicates",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/run1/frame.png", "frame.png"));
    controller.library().add(make_entry("/run2/frame.png", "frame.png"));
    controller.library().add(make_entry("/run1/unique.png", "unique.png"));

    controller.compute_display_labels();

    REQUIRE(controller.library().all()[0].display_label == "run1/frame.png");
    REQUIRE(controller.library().all()[1].display_label == "run2/frame.png");
    REQUIRE(controller.library().all()[2].display_label == "unique.png");
}

TEST_CASE("AppController::compute_display_labels is a no-op on empty library",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.compute_display_labels(); // must not crash

    REQUIRE(controller.library().all().empty());
}

TEST_CASE("AppController forwards trivial queries to its services",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // Empty selection -> both A and B are -1.
    int a = 99, b = 99;
    controller.get_ab_indices(a, b);
    REQUIRE(a == -1);
    REQUIRE(b == -1);

    // No multi-frame entries -> timeline length clamps to 1.
    REQUIRE(controller.timeline_length() == 1);

    // No SR tasks have been started.
    REQUIRE_FALSE(controller.has_running_sr_tasks());
}

TEST_CASE("AppController::sync_entries_to_timeline is silent on empty library",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.sync_entries_to_timeline();

    // No entries means sync_to() never reports anything; the reporter
    // must remain untouched and the diff must stay in its initial
    // (dirty) state without an extra mark_dirty edge.
    REQUIRE(reporter.status_calls.empty());
    REQUIRE(reporter.append_calls.empty());
}

TEST_CASE("AppController::start_sr_task surfaces missing-engine errors",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // No SR engine is registered in the test binary; the underlying
    // SrTaskService should refuse to enqueue a task and the
    // controller must forward the diagnostic to the reporter as a
    // modal error rather than as a status-bar string.
    idiff::SRTaskParams params{};
    params.input_path = "/tmp/missing.png";
    params.output_path = "/tmp/missing_sr_2x.png";

    controller.start_sr_task(params);

    REQUIRE(reporter.error_titles.size() == 1);
    REQUIRE(reporter.error_messages.size() == 1);
    REQUIRE(reporter.error_titles.front() == "Super Resolution Error");
    REQUIRE_FALSE(reporter.error_messages.front().empty());

    // Failed start must not enqueue anything.
    REQUIRE_FALSE(controller.has_running_sr_tasks());

    // Status bar untouched; only the modal channel was used.
    REQUIRE(reporter.status_calls.empty());
    REQUIRE(reporter.sr_status_calls.empty());
}

TEST_CASE("AppController::reload_all_images is silent on empty library",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.reload_all_images();

    // No entries -> no work, no status update.  This matters because
    // the View menu fires reload_all_images() on every backend toggle
    // even when no images are loaded.
    REQUIRE(reporter.status_calls.empty());
}

TEST_CASE("AppController loader_backend round-trips through the setter",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // Default matches ImageLoader::default_backend so load_images()
    // and reload_all_images() produce identical pixels until the user
    // explicitly switches.
    REQUIRE(controller.loader_backend() == idiff::ImageLoader::default_backend());

    // OpenCV is always compiled in, so it is the safe target for a
    // round-trip test that runs on every CI configuration.
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);
    REQUIRE(controller.loader_backend() == idiff::LoaderBackend::OpenCV);
}

TEST_CASE("AppController::load_images first load auto-selects up to two entries",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // Use OpenCV explicitly so the test is deterministic regardless of
    // whether ImageMagick is compiled in on this build.
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    const auto p1 = write_tmp_png("first_load", "a.png");
    const auto p2 = write_tmp_png("first_load", "b.png");
    const auto p3 = write_tmp_png("first_load", "c.png");

    auto result = controller.load_images({p1, p2, p3});

    // Library now holds three entries, sorted by filename.
    REQUIRE(controller.library().all().size() == 3);
    REQUIRE(controller.library().all()[0].filename == "a.png");
    REQUIRE(controller.library().all()[1].filename == "b.png");
    REQUIRE(controller.library().all()[2].filename == "c.png");

    // First-load convenience: the first two entries are auto-selected
    // and the result tells the caller to switch the viewport mode.
    REQUIRE(result.did_first_load_select);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});
    REQUIRE_FALSE(controller.selection().swap_ab());

    // The diff cache was marked dirty so the next render recomputes it.
    REQUIRE(controller.diff().is_dirty());

    // Per-file status was reported through the reporter; the last one
    // wins so the current text mentions one of the loaded paths.
    REQUIRE_FALSE(reporter.current_status.empty());
    REQUIRE(reporter.current_status.find("Loaded:") != std::string::npos);
}

TEST_CASE("AppController::load_images appends without disturbing selection",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    const auto a = write_tmp_png("append", "a.png");
    const auto b = write_tmp_png("append", "b.png");

    controller.load_images({a});

    // The caller pretends to manually pick a different selection set
    // (mimicking a user clicking around in the image list).
    controller.selection().clear();
    controller.selection().set_swap_ab(true);

    const auto c = write_tmp_png("append", "c.png");
    auto result = controller.load_images({b, c});

    // Library grew but the second call is not a "first load", so the
    // controller must NOT clobber the manual selection.
    REQUIRE(controller.library().all().size() == 3);
    REQUIRE_FALSE(result.did_first_load_select);
    REQUIRE(controller.selection().indices().empty());
    REQUIRE(controller.selection().swap_ab());
}

TEST_CASE("AppController::load_images reports failures via the status reporter",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    auto result = controller.load_images({"/no/such/file.png"});

    // No entries were added, so the auto-select path did not fire.
    REQUIRE(controller.library().all().empty());
    REQUIRE_FALSE(result.did_first_load_select);

    // The failure was surfaced through the status reporter so the
    // user can see what went wrong.
    REQUIRE_FALSE(reporter.current_status.empty());
    REQUIRE(reporter.current_status.find("Failed to load:") != std::string::npos);
}

TEST_CASE("AppController::load_comparison_config reports parse failure",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    auto dir = std::filesystem::temp_directory_path()
             / "idiff_ctrl_cfg_bad";
    std::filesystem::create_directories(dir);
    auto json_path = dir / "broken.json";
    {
        std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
        out << "this is not valid json";
    }
    controller.comparison_config().set_cache_root_override(dir);

    auto result = controller.load_comparison_config(json_path.string());

    // Parse failure -> service stays empty, no implicit group switch.
    REQUIRE_FALSE(result.did_first_load_select);
    REQUIRE_FALSE(controller.comparison_config().has_config());
    REQUIRE(controller.library().all().empty());

    // The reason was surfaced through the status reporter.
    REQUIRE_FALSE(reporter.current_status.empty());
}

TEST_CASE("AppController::switch_to_comparison_group drives load_images and labels",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    // Stage a real PNG that the URL cache will resolve to.  Use a
    // unique scratch dir so back-to-back runs do not collide.
    auto dir = std::filesystem::temp_directory_path()
             / "idiff_ctrl_cfg_ok";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto image_path = write_tmp_png("cfg_ok", "real.png");

    // Write a minimal config that points at the staged PNG via a
    // file:// URL so UrlCache::fetch() takes its disk fast path.
    auto json_path = dir / "ok.json";
    {
        std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
        out << R"({"groups": [{"name": "g1", "items": [
            {"url": "file:///cfgreal.png", "title": "My Label"}
        ]}]})";
    }

    // Drive load() through the service directly so we can stage the
    // URL on disk before the implicit switch_to(0) fires.  The
    // controller's load_comparison_config() helper would auto-switch
    // immediately and find nothing to fetch.
    auto& svc = controller.comparison_config();
    svc.set_cache_root_override(dir);
    REQUIRE(svc.load(json_path.string()).ok);

    // Stage real bytes at the cache path the service will look up.
    auto* cache = svc.url_cache_for_test();
    REQUIRE(cache != nullptr);
    {
        auto target = cache->path_for("file:///cfgreal.png");
        std::filesystem::create_directories(target.parent_path());
        // Copy the PNG payload so ImageFileSource can decode it.
        std::filesystem::copy_file(image_path, target,
            std::filesystem::copy_options::overwrite_existing);
    }

    auto result = controller.switch_to_comparison_group(0);

    // The fetched entry was loaded, auto-selected, and relabelled with
    // the human-friendly title from the config.
    REQUIRE(controller.library().all().size() == 1);
    REQUIRE(controller.library().all()[0].display_label == "My Label");
    REQUIRE(controller.library().all()[0].filename == "My Label");
    REQUIRE(result.did_first_load_select);

    // Service status (e.g. "Loaded group ...") was forwarded last so
    // the status bar reflects the group switch, not the per-file load.
    REQUIRE_FALSE(reporter.current_status.empty());
}

TEST_CASE("AppController::switch_to_comparison_group ignores no-op switch",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // No config has been loaded; current_index stays -1.  Asking to
    // switch to -1 matches and the call must short-circuit without
    // touching the library, the diff, or the status reporter.
    auto result = controller.switch_to_comparison_group(-1);

    REQUIRE_FALSE(result.did_first_load_select);
    REQUIRE(controller.library().all().empty());
    REQUIRE(reporter.status_calls.empty());
}
