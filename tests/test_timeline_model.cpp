// Unit tests for TimelineModel.
//
// TimelineModel owns the shared current-frame index and the
// length()/sync_to()/clamp_to_length() helpers that walk the entry
// vector.  These tests use a stub MediaSource that returns synthetic
// frames so we can verify:
//   * length() reports the maximum frame_count across multi-frame
//     entries (still images don't contribute, minimum is 1)
//   * sync_to() decodes target = current + offset, clamped per entry,
//     skipping unchanged caches and reporting any read failures
//   * clamp_to_length() pins out-of-range indices into the valid range
//
// We construct ImageEntry instances directly because TimelineModel
// only touches the fields documented in its contract (source, image,
// frame_offset, cached_frame, texture_dirty, display_image,
// filename).

#include <catch2/catch_test_macros.hpp>

#include "domain/timeline_model.h"
#include "app/app.h"
#include "core/image.h"
#include "core/media_source.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

using idiff::ImageEntry;
using idiff::Image;
using idiff::MediaSource;
using idiff::TimelineModel;

namespace {

// Minimal MediaSource stub: announces a fixed frame_count and returns
// a unique 1x1 Image per frame index (or null at the configured fail
// frame, so we can exercise the error-reporting branch).
class StubSource : public MediaSource {
public:
    StubSource(int frame_count, int fail_frame = -1)
        : frame_count_(frame_count), fail_frame_(fail_frame) {}

    int frame_count() const noexcept override { return frame_count_; }
    int width() const noexcept override { return 1; }
    int height() const noexcept override { return 1; }
    const std::string& format_description() const noexcept override {
        return format_desc_;
    }

    std::unique_ptr<Image> read_frame(int index) override {
        last_requested_ = index;
        ++read_calls_;
        if (index == fail_frame_) {
            last_error_ = "stub: forced failure";
            return nullptr;
        }
        // Caller stores this and treats it as opaque pixel data.  We
        // hand back a default-constructed Image since TimelineModel
        // never inspects pixel content; only the unique_ptr identity
        // matters.
        return std::make_unique<Image>();
    }

    const std::string& last_error() const noexcept override {
        return last_error_;
    }

    int last_requested() const { return last_requested_; }
    int read_calls() const { return read_calls_; }

private:
    int frame_count_;
    int fail_frame_;
    int last_requested_ = -1;
    int read_calls_ = 0;
    std::string last_error_;
    std::string format_desc_;
};

ImageEntry make_entry(const std::string& filename,
                      std::unique_ptr<MediaSource> source) {
    ImageEntry e;
    e.filename = filename;
    e.source = std::move(source);
    // Pre-seed image so the "cached_frame matches & image present"
    // short-circuit can fire when we expect it to.
    if (e.source) {
        auto img = e.source->read_frame(0);
        if (img) {
            e.image = std::move(img);
            e.cached_frame = 0;
        }
    }
    return e;
}

} // namespace

TEST_CASE("TimelineModel: defaults to frame 0", "[timeline_model]") {
    TimelineModel tl;
    REQUIRE(tl.current_frame() == 0);
}

TEST_CASE("TimelineModel: length is 1 for an empty entry list",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    REQUIRE(TimelineModel::length(entries) == 1);
}

TEST_CASE("TimelineModel: length is 1 when no entry has multiple frames",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("still_a", std::make_unique<StubSource>(1)));
    entries.push_back(make_entry("still_b", std::make_unique<StubSource>(1)));
    REQUIRE(TimelineModel::length(entries) == 1);
}

TEST_CASE("TimelineModel: length returns max frame_count across entries",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("short", std::make_unique<StubSource>(5)));
    entries.push_back(make_entry("long",  std::make_unique<StubSource>(42)));
    entries.push_back(make_entry("still", std::make_unique<StubSource>(1)));
    REQUIRE(TimelineModel::length(entries) == 42);
}

TEST_CASE("TimelineModel: sync_to skips still images",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    auto stub_owned = std::make_unique<StubSource>(1);
    StubSource* stub = stub_owned.get();
    entries.push_back(make_entry("still", std::move(stub_owned)));
    const int reads_after_make_entry = stub->read_calls();

    TimelineModel tl;
    tl.set_current_frame(7);  // shouldn't matter for a 1-frame source
    std::string status;
    REQUIRE_FALSE(tl.sync_to(entries, status));
    REQUIRE(status.empty());
    // sync_to must not have called read_frame on the still image.
    REQUIRE(stub->read_calls() == reads_after_make_entry);
}

TEST_CASE("TimelineModel: sync_to advances multi-frame entries",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("clip", std::make_unique<StubSource>(10)));

    TimelineModel tl;
    tl.set_current_frame(3);
    std::string status;
    REQUIRE(tl.sync_to(entries, status));
    REQUIRE(status.empty());
    REQUIRE(entries[0].cached_frame == 3);
    REQUIRE(entries[0].texture_dirty);
}

TEST_CASE("TimelineModel: sync_to applies per-entry frame_offset and clamps",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("clip", std::make_unique<StubSource>(10)));
    entries[0].frame_offset = 5;
    entries[0].texture_dirty = false;

    TimelineModel tl;
    tl.set_current_frame(8);  // 8 + 5 = 13, clamped to 9
    std::string status;
    REQUIRE(tl.sync_to(entries, status));
    REQUIRE(entries[0].cached_frame == 9);
    REQUIRE(entries[0].texture_dirty);

    // Negative effective frame clamps to 0.
    entries[0].frame_offset = -100;
    entries[0].texture_dirty = false;
    REQUIRE(tl.sync_to(entries, status));
    REQUIRE(entries[0].cached_frame == 0);
}

TEST_CASE("TimelineModel: sync_to is a no-op when cached frame matches",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    auto stub_owned = std::make_unique<StubSource>(10);
    StubSource* stub = stub_owned.get();
    entries.push_back(make_entry("clip", std::move(stub_owned)));
    // make_entry already pre-decoded frame 0 once.
    const int initial_reads = stub->read_calls();

    TimelineModel tl;  // current_frame = 0, matches cached_frame
    entries[0].texture_dirty = false;
    std::string status;
    REQUIRE_FALSE(tl.sync_to(entries, status));
    REQUIRE(stub->read_calls() == initial_reads);
    REQUIRE_FALSE(entries[0].texture_dirty);
}

TEST_CASE("TimelineModel: sync_to reports per-entry read failures",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    // Source fails when asked for frame 3.
    entries.push_back(
        make_entry("flaky", std::make_unique<StubSource>(10, /*fail_frame=*/3)));
    // Pre-existing image must survive a failed read.
    Image* original = entries[0].image.get();

    TimelineModel tl;
    tl.set_current_frame(3);
    std::string status;
    REQUIRE_FALSE(tl.sync_to(entries, status));
    REQUIRE(status.find("flaky") != std::string::npos);
    REQUIRE(status.find("forced failure") != std::string::npos);
    REQUIRE(entries[0].image.get() == original);
    REQUIRE(entries[0].cached_frame == 0);  // unchanged
}

TEST_CASE("TimelineModel: sync_to appends multiple failures with separator",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    entries.push_back(
        make_entry("a", std::make_unique<StubSource>(10, /*fail_frame=*/2)));
    entries.push_back(
        make_entry("b", std::make_unique<StubSource>(10, /*fail_frame=*/2)));

    TimelineModel tl;
    tl.set_current_frame(2);
    std::string status;
    tl.sync_to(entries, status);
    REQUIRE(status.find("a") != std::string::npos);
    REQUIRE(status.find("b") != std::string::npos);
    REQUIRE(status.find(" | ") != std::string::npos);
}

TEST_CASE("TimelineModel: clamp_to_length pins indices into [0, len-1]",
          "[timeline_model]") {
    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("clip", std::make_unique<StubSource>(10)));

    TimelineModel tl;
    tl.set_current_frame(50);
    REQUIRE(tl.clamp_to_length(entries));
    REQUIRE(tl.current_frame() == 9);

    tl.set_current_frame(-3);
    REQUIRE(tl.clamp_to_length(entries));
    REQUIRE(tl.current_frame() == 0);

    REQUIRE_FALSE(tl.clamp_to_length(entries));  // already in range
    REQUIRE(tl.current_frame() == 0);
}
