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
#include "app/app.h"
#include "domain/diff_service.h"
#include "domain/image_library.h"
#include "domain/selection_model.h"
#include "core/image.h"        // IWYU pragma: keep
#include "core/media_source.h" // IWYU pragma: keep

#include <cstdint>
#include <string>

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

idiff::ImageEntry make_entry(const std::string& path,
                             const std::string& filename) {
    idiff::ImageEntry e;
    e.path = path;
    e.filename = filename;
    e.display_label = filename;
    return e;
}

} // namespace

TEST_CASE("AppController::remove_entry coordinates library, selection, diff",
          "[controller]") {
    CountingUploader uploader;
    idiff::AppController controller(uploader);

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
    idiff::AppController controller(uploader);

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
    idiff::AppController controller(uploader);

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
    idiff::AppController controller(uploader);

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
    idiff::AppController controller(uploader);

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
    idiff::AppController controller(uploader);

    controller.compute_display_labels(); // must not crash

    REQUIRE(controller.library().all().empty());
}

TEST_CASE("AppController forwards trivial queries to its services",
          "[controller]") {
    CountingUploader uploader;
    idiff::AppController controller(uploader);

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
