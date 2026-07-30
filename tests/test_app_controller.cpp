// Smoke tests for AppController orchestration helpers.
//
// AppController now owns every domain service and exposes a handful of
// composite operations that used to live inline on App (and were
// therefore unreachable from any test that did not bring up SDL +
// ImGui).  These tests exercise the cross-service contracts that App
// previously relied on:
//
//   * remove_entry triggers library removal, selection remap,
//     label recomputation and a diff dirty mark in one shot.
//   * sort_entries_by_name and move_entry both keep the selection in
//     sync with the new positions.
//   * compute_display_labels shows each entry's parent directory (shortest
//     distinguishing path suffix) so origins stay visible, but preserves
//     custom labels such as comparison-config titles.
//   * timeline_length, has_running_sr_tasks and get_ref_index forward
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

    controller.remove_entry(0);

    // x.png is gone; y.png and z.png remain at indices 0 and 1.
    REQUIRE(controller.library().all().size() == 2);
    REQUIRE(controller.library().all()[0].filename == "y.png");
    REQUIRE(controller.library().all()[1].filename == "z.png");

    // The previously-selected indices were {0, 2}; 0 dropped, 2 -> 1.
    REQUIRE(controller.selection().indices().size() == 1);
    REQUIRE(controller.selection().indices().count(1) == 1);

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

TEST_CASE("AppController::mark_as_reference sets reference without moving entry",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/p/a.png", "a.png"));
    controller.library().add(make_entry("/p/b.png", "b.png"));
    controller.library().add(make_entry("/p/c.png", "c.png"));
    controller.selection().insert(0);
    controller.selection().insert(1);

    controller.mark_as_reference(2);

    // Library order is unchanged -- entries stay where they are.
    REQUIRE(controller.library().all()[0].filename == "a.png");
    REQUIRE(controller.library().all()[1].filename == "b.png");
    REQUIRE(controller.library().all()[2].filename == "c.png");

    // The entry is added to the selection and designated as reference.
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1, 2});
    REQUIRE(controller.selection().has_explicit_reference());

    int ref = -1;
    controller.get_ref_index(ref);
    REQUIRE(ref == 2);
    REQUIRE(controller.library().all()[ref].filename == "c.png");

    // Diff cache must be dirty so the next render recomputes against
    // the new reference.
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::mark_as_reference on an already-selected entry",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/p/a.png", "a.png"));
    controller.library().add(make_entry("/p/b.png", "b.png"));

    // a.png is at index 0, not yet selected.
    controller.mark_as_reference(0);

    // Library order is unchanged; a.png is now selected and the
    // explicit reference.
    REQUIRE(controller.library().all()[0].filename == "a.png");
    REQUIRE(controller.library().all()[1].filename == "b.png");
    REQUIRE(controller.selection().indices() == std::set<int>{0});
    REQUIRE(controller.selection().has_explicit_reference());

    int ref = -1;
    controller.get_ref_index(ref);
    REQUIRE(ref == 0);
}

TEST_CASE("AppController::mark_as_reference ignores out-of-range indices",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/p/a.png", "a.png"));
    controller.selection().insert(0);

    controller.mark_as_reference(-1);
    controller.mark_as_reference(99);

    // Library and selection are unchanged.
    REQUIRE(controller.library().all().size() == 1);
    REQUIRE(controller.selection().indices() == std::set<int>{0});
}

TEST_CASE("AppController per-comparison reference persists across switches",
          "[controller][comparison_reference]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // Two comparisons by filename stem: "a" and "b".  Two
    // directories so we can verify reference-path tracking is
    // directory-aware.
    controller.library().add(make_entry("/Foo/a.png", "a.png"));
    controller.library().add(make_entry("/Bar/a.png", "a.png"));
    controller.library().add(make_entry("/Foo/b.png", "b.png"));
    controller.library().add(make_entry("/Bar/b.png", "b.png"));

    // Activate comparison "a" (indices 0, 1) and pin /Bar/a.png as
    // reference.
    REQUIRE(controller.select_group(0));
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});
    controller.mark_as_reference(1);
    int ref = -1;
    controller.get_ref_index(ref);
    REQUIRE(ref == 1);

    // Switch to comparison "b" (indices 2, 3); reference is the
    // implicit smallest selected index (no rule recorded for
    // comparison "b" yet).
    REQUIRE(controller.select_group(2));
    REQUIRE(controller.selection().indices() == std::set<int>{2, 3});
    controller.get_ref_index(ref);
    REQUIRE(ref == 2);
    REQUIRE_FALSE(controller.selection().has_explicit_reference());

    // Pin /Bar/b.png as comparison "b" reference and switch back
    // to "a".
    controller.mark_as_reference(3);
    REQUIRE(controller.select_group(0));
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});
    controller.get_ref_index(ref);
    // Comparison "a" should remember its pinned reference (/Bar/a.png).
    REQUIRE(ref == 1);
    REQUIRE(controller.selection().has_explicit_reference());

    // And comparison "b" should remember its pinned reference
    // (/Bar/b.png).
    REQUIRE(controller.select_group(2));
    controller.get_ref_index(ref);
    REQUIRE(ref == 3);
}

TEST_CASE("AppController::list_comparisons exposes file-stem comparisons",
          "[controller][comparison_reference]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/Foo/a.png", "a.png"));
    controller.library().add(make_entry("/Bar/a.png", "a.png"));
    controller.library().add(make_entry("/Foo/b.png", "b.png"));

    auto comparisons = controller.list_comparisons();
    REQUIRE(comparisons.size() == 2);

    // Order is library-insertion-order of first occurrence.
    REQUIRE(comparisons[0].key == "file:a");
    REQUIRE(comparisons[0].entries == std::vector<int>{0, 1});
    REQUIRE(comparisons[0].current);

    REQUIRE(comparisons[1].key == "file:b");
    REQUIRE(comparisons[1].entries == std::vector<int>{2});
}

TEST_CASE("AppController::set_comparison_reference applies on next activation",
          "[controller][comparison_reference]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/Foo/a.png", "a.png"));
    controller.library().add(make_entry("/Bar/a.png", "a.png"));
    controller.library().add(make_entry("/Foo/b.png", "b.png"));
    controller.library().add(make_entry("/Bar/b.png", "b.png"));

    // Record a reference for comparison "b" without ever activating it.
    REQUIRE(controller.set_comparison_reference("file:b", "/Bar/b.png"));

    // Activating comparison "b" should auto-mark /Bar/b.png as reference.
    REQUIRE(controller.select_group(2));
    int ref = -1;
    controller.get_ref_index(ref);
    REQUIRE(ref == 3);
    REQUIRE(controller.selection().has_explicit_reference());
}

TEST_CASE("AppController::set_comparison_reference is robust to missing path",
          "[controller][comparison_reference]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/Foo/a.png", "a.png"));
    controller.library().add(make_entry("/Bar/a.png", "a.png"));

    // Record a path that does not appear in any selected entry.
    REQUIRE(controller.set_comparison_reference("file:a", "/nowhere/a.png"));

    REQUIRE(controller.select_group(0));
    int ref = -1;
    controller.get_ref_index(ref);
    // Falls through to the implicit smallest-index rule.
    REQUIRE(ref == 0);
    REQUIRE_FALSE(controller.selection().has_explicit_reference());
}

TEST_CASE("AppController::set_comparison_reference rejects empty key",
          "[controller][comparison_reference]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    REQUIRE_FALSE(controller.set_comparison_reference("", "/x.png"));
    REQUIRE(controller.comparison_references().empty());
}

TEST_CASE("AppController::mark_as_reference narrows cross-comparison selection",
          "[controller][comparison_reference]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // Two comparisons (stems "a" and "b") spread across two
    // directories.  Mimics the AI-driven workflow where an agent
    // calls library.set_reference once per file in a target
    // directory: each call lands in a different comparison.
    controller.library().add(make_entry("/Foo/a.png", "a.png"));
    controller.library().add(make_entry("/Bar/a.png", "a.png"));
    controller.library().add(make_entry("/Foo/b.png", "b.png"));
    controller.library().add(make_entry("/Bar/b.png", "b.png"));

    // Activate comparison "a" first (mimicking GUI / first-load
    // selection).
    REQUIRE(controller.select_group(0));
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});

    // Mark /Foo/a.png as reference for comparison "a".  Selection
    // stays in comparison "a" -- same comparison, just a reference
    // pin within the active set.
    controller.mark_as_reference(0);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});
    int ref = -1;
    controller.get_ref_index(ref);
    REQUIRE(ref == 0);

    // Now mark /Foo/b.png (in comparison "b") as reference.  The
    // selection must narrow to comparison "b" rather than spilling
    // across both comparisons -- otherwise the viewport ends up
    // diffing /Foo/a.png against /Bar/b.png, which is meaningless.
    controller.mark_as_reference(2);
    REQUIRE(controller.selection().indices() == std::set<int>{2, 3});
    controller.get_ref_index(ref);
    REQUIRE(ref == 2);

    // Both per-comparison references were recorded so revisiting
    // either comparison restores its pin.
    REQUIRE(controller.comparison_references().at("file:a")
            == "/Foo/a.png");
    REQUIRE(controller.comparison_references().at("file:b")
            == "/Foo/b.png");

    // Switching back to comparison "a" via select_group restores
    // its pinned reference.
    REQUIRE(controller.select_group(0));
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});
    controller.get_ref_index(ref);
    REQUIRE(ref == 0);
}

TEST_CASE("AppController per-comparison reference survives entry removal",
          "[controller][comparison_reference]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/Foo/a.png", "a.png"));
    controller.library().add(make_entry("/Bar/a.png", "a.png"));

    REQUIRE(controller.select_group(0));
    controller.mark_as_reference(1);

    // Remove the pinned entry.  The recorded mapping points at a
    // path that is no longer present; the next activation should
    // fall through to the implicit reference without crashing.
    controller.remove_entry(1);
    controller.select_group(0);  // selection already covers comparison "a"
    int ref = -1;
    controller.get_ref_index(ref);
    REQUIRE(ref == 0);
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
    REQUIRE(controller.library().all()[2].display_label == "run1/unique.png");
}

// Regression: entries that share a stem but differ only by extension
// (e.g. output/foo.jpeg vs input/foo.jpg) must still show their parent
// directory.  Filename-only disambiguation missed this because the
// filenames are technically distinct, yet the user cannot tell which
// directory each came from.
TEST_CASE("AppController::compute_display_labels shows parent dir for same-stem "
          "different-extension entries",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(
        make_entry("/Volumes/aigc/output/foo.jpeg", "foo.jpeg"));
    controller.library().add(
        make_entry("/Volumes/aigc/input/foo.jpg", "foo.jpg"));

    controller.compute_display_labels();

    REQUIRE(controller.library().all()[0].display_label == "output/foo.jpeg");
    REQUIRE(controller.library().all()[1].display_label == "input/foo.jpg");
}

// Regression: when the immediate parent directory is shared, the label
// must grow further up the tree to the shortest component that actually
// distinguishes the entries (foo/aaa vs bar/aaa), not just the nearest
// directory.  This is the case the original filename-only logic never
// handled -- two same-named files under the same subfolder in different
// roots.
TEST_CASE("AppController::compute_display_labels grows past a shared parent",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/foo/aaa/1.png", "1.png"));
    controller.library().add(make_entry("/bar/aaa/1.png", "1.png"));

    controller.compute_display_labels();

    REQUIRE(controller.library().all()[0].display_label == "foo/aaa/1.png");
    REQUIRE(controller.library().all()[1].display_label == "bar/aaa/1.png");
}

// Regression: the shortest distinguishing suffix is kept -- only as many
// directory components as needed for uniqueness, never the full path.
TEST_CASE("AppController::compute_display_labels keeps shortest distinguishing "
          "path suffix",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/b/c/1.png", "1.png"));
    controller.library().add(make_entry("/a/b/d/1.png", "1.png"));
    controller.library().add(make_entry("/x/y/z/2.png", "2.png"));

    controller.compute_display_labels();

    REQUIRE(controller.library().all()[0].display_label == "c/1.png");
    REQUIRE(controller.library().all()[1].display_label == "d/1.png");
    REQUIRE(controller.library().all()[2].display_label == "z/2.png");
}

// Regression: a mix of shared and distinct immediate parents -- the
// distinguishing depth differs per entry, so each gets the minimal suffix
// it needs (foo/aaa vs bar/aaa for the shared 1.png, but just z for 2.png).
TEST_CASE("AppController::compute_display_labels uses per-entry minimal depth",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/foo/aaa/1.png", "1.png"));
    controller.library().add(make_entry("/bar/aaa/1.png", "1.png"));
    controller.library().add(make_entry("/x/y/z/2.png", "2.png"));

    controller.compute_display_labels();

    REQUIRE(controller.library().all()[0].display_label == "foo/aaa/1.png");
    REQUIRE(controller.library().all()[1].display_label == "bar/aaa/1.png");
    REQUIRE(controller.library().all()[2].display_label == "z/2.png");
}

// Regression: the "(N frames)" suffix on multi-frame entries must survive
// compute_display_labels().  Previously the suffix was baked into
// display_label and got overwritten when the path-derived label was
// rebuilt.
TEST_CASE("AppController::compute_display_labels preserves frame-count suffix",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    auto e = make_entry("/run1/clip.yuv", "clip.yuv");
    e.label_suffix = " (300 frames)";
    controller.library().add(std::move(e));
    controller.library().add(make_entry("/run2/clip.yuv", "clip.yuv"));

    controller.compute_display_labels();

    REQUIRE(controller.library().all()[0].display_label == "run1/clip.yuv (300 frames)");
    REQUIRE(controller.library().all()[1].display_label == "run2/clip.yuv");
}

// Regression: entries flagged label_custom (SR result names, comparison-
// config titles) must keep their display_label verbatim even when
// compute_display_labels() runs after another image is loaded.
TEST_CASE("AppController::compute_display_labels preserves custom labels",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    auto sr = make_entry("/cache/out_sr_2x.png", "out_sr_2x.png");
    sr.display_label = "input.png (SR 2x)";
    sr.label_custom = true;
    controller.library().add(std::move(sr));
    controller.library().add(make_entry("/cache/input.png", "input.png"));

    controller.compute_display_labels();

    REQUIRE(controller.library().all()[0].display_label == "input.png (SR 2x)");
    REQUIRE(controller.library().all()[1].display_label == "cache/input.png");
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

    // Empty selection -> reference is -1.
    int ref = 99;
    controller.get_ref_index(ref);
    REQUIRE(ref == -1);

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

    const auto c = write_tmp_png("append", "c.png");
    auto result = controller.load_images({b, c});

    // Library grew but the second call is not a "first load", so the
    // controller must NOT clobber the manual selection.
    REQUIRE(controller.library().all().size() == 3);
    REQUIRE_FALSE(result.did_first_load_select);
    REQUIRE(controller.selection().indices().empty());
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

    // It must also raise a modal so a status-bar glance is not the
    // only chance the user has to notice the failure.
    REQUIRE(reporter.error_titles.size() == 1);
    REQUIRE(reporter.error_titles.front().find("Failed to load")
            != std::string::npos);
    REQUIRE(reporter.error_messages.size() == 1);
    REQUIRE(reporter.error_messages.front().find("/no/such/file.png")
            != std::string::npos);
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

TEST_CASE("AppController::reload_entry refreshes a single entry from disk",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    const auto p1 = write_tmp_png("reload_one", "a.png", 50);
    const auto p2 = write_tmp_png("reload_one", "b.png", 100);
    controller.load_images({p1, p2});

    // Clear dirty flags so we can detect reload side effects.
    for (auto& e : controller.library().all()) {
        e.texture_dirty = false;
    }

    controller.reload_entry(0);

    // Only the target entry is re-decoded and flagged dirty.
    REQUIRE(controller.library().all()[0].texture_dirty);
    REQUIRE_FALSE(controller.library().all()[1].texture_dirty);
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::reload_entry ignores out-of-range index",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    const auto p = write_tmp_png("reload_oob", "only.png");
    controller.load_images({p});

    const auto status_before = reporter.status_calls.size();

    controller.reload_entry(-1);
    controller.reload_entry(99);

    // No crash, no extra status messages beyond the load.
    REQUIRE(controller.library().all().size() == 1);
    REQUIRE(reporter.status_calls.size() == status_before);
}

TEST_CASE("AppController::reload_entries_by_path batch-reloads matched paths",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    const auto p1 = write_tmp_png("reload_batch", "x.png", 10);
    const auto p2 = write_tmp_png("reload_batch", "y.png", 20);
    const auto p3 = write_tmp_png("reload_batch", "z.png", 30);
    controller.load_images({p1, p2, p3});

    for (auto& e : controller.library().all()) {
        e.texture_dirty = false;
    }

    // Reload only two of the three.
    int reloaded = controller.reload_entries_by_path({p1, p3});

    REQUIRE(reloaded == 2);
    REQUIRE(controller.library().all()[0].texture_dirty);   // x.png
    REQUIRE_FALSE(controller.library().all()[1].texture_dirty); // y.png
    REQUIRE(controller.library().all()[2].texture_dirty);   // z.png
    REQUIRE(controller.diff().is_dirty());

    // Status message reports the reload count.
    REQUIRE_FALSE(reporter.status_calls.empty());
    REQUIRE(reporter.status_calls.back().find("2") != std::string::npos);
}

TEST_CASE("AppController::reload_entries_by_path skips unknown paths",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    const auto p = write_tmp_png("reload_skip", "real.png");
    controller.load_images({p});

    const auto status_before = reporter.status_calls.size();
    int reloaded = controller.reload_entries_by_path(
        {"/no/such/file.png", "/another/missing.png"});

    REQUIRE(reloaded == 0);
    // No extra status when nothing was reloaded.
    REQUIRE(reporter.status_calls.size() == status_before);
}

TEST_CASE("AppController::load_images deduplicates existing paths",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    const auto p1 = write_tmp_png("dedup", "img.png", 80);
    controller.load_images({p1});

    REQUIRE(controller.library().all().size() == 1);

    // Load the same path again -- should refresh, not duplicate.
    auto result = controller.load_images({p1});

    REQUIRE(controller.library().all().size() == 1);
    REQUIRE(controller.library().all()[0].texture_dirty);

    // The "Refreshed:" status message confirms dedup happened.
    bool found_refresh = false;
    for (const auto& s : reporter.status_calls) {
        if (s.find("Refreshed:") != std::string::npos) {
            found_refresh = true;
            break;
        }
    }
    REQUIRE(found_refresh);
}

TEST_CASE("AppController::load_images dedup mixed with new paths",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);
    controller.set_loader_backend(idiff::LoaderBackend::OpenCV);

    const auto p1 = write_tmp_png("dedup_mix", "first.png", 10);
    controller.load_images({p1});
    REQUIRE(controller.library().all().size() == 1);

    // p1 already exists, p2 is new.
    const auto p2 = write_tmp_png("dedup_mix", "second.png", 20);
    controller.load_images({p1, p2});

    // Should have exactly 2 entries, not 3.
    REQUIRE(controller.library().all().size() == 2);
}

TEST_CASE("AppController::group_indices returns matching entries by stem",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // kid.jpg and kid.png share the stem "kid"; other.png is alone.
    controller.library().add(make_entry("/a/kid.jpg", "kid.jpg"));
    controller.library().add(make_entry("/b/kid.png", "kid.png"));
    controller.library().add(make_entry("/c/other.png", "other.png"));

    auto group = controller.group_indices(0);
    REQUIRE(group == std::set<int>{0, 1});

    group = controller.group_indices(1);
    REQUIRE(group == std::set<int>{0, 1});

    group = controller.group_indices(2);
    REQUIRE(group == std::set<int>{2});
}

TEST_CASE("AppController::group_indices returns empty for out-of-range",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/x.png", "x.png"));

    REQUIRE(controller.group_indices(-1).empty());
    REQUIRE(controller.group_indices(99).empty());
}

TEST_CASE("AppController::select_group replaces selection with group",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/kid.jpg", "kid.jpg"));
    controller.library().add(make_entry("/b/kid.png", "kid.png"));
    controller.library().add(make_entry("/c/other.png", "other.png"));

    // Start with "other" selected, then switch to kid's group.
    controller.selection().insert(2);
    bool changed = controller.select_group(0);

    REQUIRE(changed);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::select_group no-op when already selected",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/kid.jpg", "kid.jpg"));
    controller.library().add(make_entry("/b/kid.png", "kid.png"));

    controller.selection().insert(0);
    controller.selection().insert(1);

    bool changed = controller.select_group(0);
    REQUIRE_FALSE(changed);
}

TEST_CASE("AppController::click_in_group switches and selects all when "
          "selection is empty",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/kid.jpg", "kid.jpg"));
    controller.library().add(make_entry("/b/kid.png", "kid.png"));
    controller.library().add(make_entry("/c/other.png", "other.png"));

    auto action = controller.click_in_group(0);
    REQUIRE(action == idiff::AppController::GroupClickAction::Switched);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::click_in_group switches when clicking a foreign "
          "group",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/kid.jpg", "kid.jpg"));
    controller.library().add(make_entry("/b/kid.png", "kid.png"));
    controller.library().add(make_entry("/c/other.jpg", "other.jpg"));
    controller.library().add(make_entry("/d/other.png", "other.png"));

    // Active selection is the "kid" group; clicking inside the
    // "other" group must switch and select the whole new group.
    controller.selection().insert(0);
    controller.selection().insert(1);

    auto action = controller.click_in_group(2);
    REQUIRE(action == idiff::AppController::GroupClickAction::Switched);
    REQUIRE(controller.selection().indices() == std::set<int>{2, 3});
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::click_in_group toggles a single entry within the "
          "active group",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    // Four images in the "kid" group plus one outsider.
    controller.library().add(make_entry("/a/kid.jpg", "kid.jpg"));
    controller.library().add(make_entry("/b/kid.png", "kid.png"));
    controller.library().add(make_entry("/c/kid.bmp", "kid.bmp"));
    controller.library().add(make_entry("/d/kid.tif", "kid.tif"));
    controller.library().add(make_entry("/e/other.png", "other.png"));

    // Start with the entire "kid" group selected.
    controller.selection().insert(0);
    controller.selection().insert(1);
    controller.selection().insert(2);
    controller.selection().insert(3);

    // Clicking the third image must remove just that one, not the
    // whole group.
    auto action = controller.click_in_group(2);
    REQUIRE(action == idiff::AppController::GroupClickAction::Toggled);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1, 3});
    REQUIRE(controller.diff().is_dirty());

    // Clicking it again brings it back without affecting the others.
    action = controller.click_in_group(2);
    REQUIRE(action == idiff::AppController::GroupClickAction::Toggled);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1, 2, 3});
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::click_in_group toggles within a partially "
          "selected active group",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/kid.jpg", "kid.jpg"));
    controller.library().add(make_entry("/b/kid.png", "kid.png"));
    controller.library().add(make_entry("/c/kid.bmp", "kid.bmp"));

    // Only one image of the group is selected; another image in the
    // same group should still be treated as "inside" and toggle.
    controller.selection().insert(0);

    auto action = controller.click_in_group(2);
    REQUIRE(action == idiff::AppController::GroupClickAction::Toggled);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 2});
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::click_in_group is a no-op for out-of-range",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a/kid.jpg", "kid.jpg"));
    controller.selection().insert(0);

    auto action = controller.click_in_group(99);
    REQUIRE(action == idiff::AppController::GroupClickAction::Noop);
    REQUIRE(controller.selection().indices() == std::set<int>{0});
}

TEST_CASE("AppController::click_in_group groups path-style filenames "
          "by basename stem",
          "[controller]") {
    // Reproduces the comparison-config flow where ImageEntry::filename
    // is overridden with a "subdir/name.ext" display label.  The two
    // variants of "role1" (one per subdir) must group together so the
    // user can switch between groups by clicking any image.
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(
        make_entry("/cache/a", "seedvr2_1x/role1.png"));
    controller.library().add(make_entry("/cache/b", "4k/role1.png"));
    controller.library().add(
        make_entry("/cache/c", "seedvr2_1x/role2.png"));
    controller.library().add(make_entry("/cache/d", "4k/role2.png"));

    // Both role1 variants should belong to the same group.
    REQUIRE(controller.group_indices(0) == std::set<int>{0, 1});
    REQUIRE(controller.group_indices(1) == std::set<int>{0, 1});
    REQUIRE(controller.group_indices(2) == std::set<int>{2, 3});

    // Active selection: one variant of role1.  Clicking any role2
    // entry must replace the selection with both role2 variants
    // (cross-group switch), not append to the existing selection.
    controller.selection().insert(0);
    auto action = controller.click_in_group(2);
    REQUIRE(action == idiff::AppController::GroupClickAction::Switched);
    REQUIRE(controller.selection().indices() == std::set<int>{2, 3});

    // Clicking the other role2 variant stays in the same group, so
    // we get a single-entry toggle instead of a fresh switch.
    action = controller.click_in_group(3);
    REQUIRE(action == idiff::AppController::GroupClickAction::Toggled);
    REQUIRE(controller.selection().indices() == std::set<int>{2});
}

TEST_CASE("AppController::select_range replaces selection",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a.png", "a.png"));
    controller.library().add(make_entry("/b.png", "b.png"));
    controller.library().add(make_entry("/c.png", "c.png"));
    controller.library().add(make_entry("/d.png", "d.png"));

    bool changed = controller.select_range(1, 3);
    REQUIRE(changed);
    REQUIRE(controller.selection().indices() == std::set<int>{1, 2, 3});
    REQUIRE(controller.diff().is_dirty());
}

TEST_CASE("AppController::select_range swaps reversed indices",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a.png", "a.png"));
    controller.library().add(make_entry("/b.png", "b.png"));
    controller.library().add(make_entry("/c.png", "c.png"));

    bool changed = controller.select_range(2, 0);
    REQUIRE(changed);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1, 2});
}

TEST_CASE("AppController::select_range clamps out-of-range indices",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    controller.library().add(make_entry("/a.png", "a.png"));
    controller.library().add(make_entry("/b.png", "b.png"));

    bool changed = controller.select_range(-5, 99);
    REQUIRE(changed);
    REQUIRE(controller.selection().indices() == std::set<int>{0, 1});
}

TEST_CASE("AppController::select_range no-op on empty library",
          "[controller]") {
    CountingUploader uploader;
    RecordingStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    bool changed = controller.select_range(0, 5);
    REQUIRE_FALSE(changed);
}
