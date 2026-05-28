// Unit tests for DiffService.
//
// DiffService owns the diff/heatmap cache used by Difference mode.
// These tests cover:
//   * empty / single-selection states produce no slots
//   * partner ordering: every selected entry except the reference
//     (smallest selected index), in natural selection order
//   * texture lifecycle: every uploaded texture is destroyed exactly
//     once when the cache is rebuilt, cleared, or the service is
//     destroyed
//   * mark_dirty() / is_dirty() / clear() semantics
//   * a single per-partner failure (missing pixel data) does not blank
//     the rest of the partners
//   * options propagate (amplification != default)
//
// SDL_Texture* is treated as an opaque cookie via the shared
// FakeUploader pattern (same approach as test_image_library).

#include <catch2/catch_test_macros.hpp>

#include "domain/diff_service.h"
#include "domain/selection_model.h"
#include "app/io/texture_uploader.h"
#include "app/app.h"
#include "core/image.h"
#include "core/image_impl.h"
#include "core/media_source.h"
#include "core/image_comparator.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using idiff::DiffService;
using idiff::Image;
using idiff::ImageEntry;
using idiff::PixelFormat;
using idiff::SelectionModel;

namespace {

class FakeUploader : public idiff::ITextureUploader {
public:
    SDL_Texture* upload(const idiff::UploadRequest& /*req*/) override {
        ++upload_count_;
        auto cookie = reinterpret_cast<SDL_Texture*>(
            static_cast<std::uintptr_t>(++next_cookie_));
        live_.insert(cookie);
        return cookie;
    }

    void destroy(SDL_Texture* tex) override {
        if (!tex) return;
        ++destroy_count_;
        live_.erase(tex);
    }

    int upload_count() const { return upload_count_; }
    int destroy_count() const { return destroy_count_; }
    std::size_t live_count() const { return live_.size(); }

private:
    int upload_count_ = 0;
    int destroy_count_ = 0;
    std::uintptr_t next_cookie_ = 0;
    std::unordered_set<SDL_Texture*> live_;
};

std::unique_ptr<Image> make_image(int w, int h, cv::Scalar fill) {
    auto img = std::make_unique<Image>();
    img->internal().mat = cv::Mat(h, w, CV_8UC3, fill);
    img->internal().info.width = w;
    img->internal().info.height = h;
    img->internal().info.pixel_format = PixelFormat::RGB8;
    img->internal().info.bit_depth = 8;
    img->internal().info.has_alpha = false;
    return img;
}

ImageEntry make_entry(const std::string& name, int w, int h, cv::Scalar fill) {
    ImageEntry e;
    e.path = "/tmp/" + name;
    e.filename = name;
    e.display_label = name;
    e.image = make_image(w, h, fill);
    return e;
}

ImageEntry make_empty_entry(const std::string& name) {
    ImageEntry e;
    e.path = "/tmp/" + name;
    e.filename = name;
    e.display_label = name;
    // no image: forces compute_difference to fail downstream
    return e;
}

} // namespace

TEST_CASE("DiffService starts dirty with empty slots", "[diff_service]") {
    FakeUploader up;
    DiffService svc(up);

    REQUIRE(svc.is_dirty());
    REQUIRE(svc.empty());
    REQUIRE(svc.size() == 0);
    REQUIRE(svc.slots().empty());
}

TEST_CASE("DiffService::update produces no slots when fewer than two selected",
          "[diff_service]") {
    FakeUploader up;
    DiffService svc(up);
    SelectionModel sel;

    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("a.png", 8, 8, cv::Scalar(10, 20, 30)));
    entries.push_back(make_entry("b.png", 8, 8, cv::Scalar(40, 50, 60)));

    std::string err;

    // No selection at all.
    svc.update(entries, sel, {}, err);
    REQUIRE(svc.empty());
    REQUIRE(!svc.is_dirty());
    REQUIRE(err.empty());

    // Single-entry selection.
    sel.insert(0);
    svc.mark_dirty();
    svc.update(entries, sel, {}, err);
    REQUIRE(svc.empty());
    REQUIRE(!svc.is_dirty());
    REQUIRE(up.upload_count() == 0);
}

TEST_CASE("DiffService::update produces one slot per partner in natural order",
          "[diff_service]") {
    FakeUploader up;
    DiffService svc(up);
    SelectionModel sel;

    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("a.png", 8, 8, cv::Scalar(10, 20, 30)));
    entries.push_back(make_entry("b.png", 8, 8, cv::Scalar(40, 50, 60)));
    entries.push_back(make_entry("c.png", 8, 8, cv::Scalar(70, 80, 90)));
    entries.push_back(make_entry("d.png", 8, 8, cv::Scalar(100, 110, 120)));

    // Select a, b, d -- but insert order is a, d, b so the natural
    // order from selection.indices() (which is a std::set) is 0, 1, 3.
    // The reference is 0 (smallest), so the partner list in slot order
    // is [1, 3].
    sel.insert(0);
    sel.insert(1);
    sel.insert(3);

    std::string err;
    svc.update(entries, sel, {}, err);

    REQUIRE(svc.size() == 2);
    REQUIRE(!svc.is_dirty());
    REQUIRE(err.empty());
    REQUIRE(svc.slots()[0].partner_entry_idx == 1);
    REQUIRE(svc.slots()[1].partner_entry_idx == 3);
    REQUIRE(svc.slots()[0].texture != nullptr);
    REQUIRE(svc.slots()[1].texture != nullptr);
    REQUIRE(up.upload_count() == 2);
}

TEST_CASE("DiffService::update destroys old textures before recomputing",
          "[diff_service]") {
    FakeUploader up;
    DiffService svc(up);
    SelectionModel sel;

    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("a.png", 8, 8, cv::Scalar(10, 20, 30)));
    entries.push_back(make_entry("b.png", 8, 8, cv::Scalar(40, 50, 60)));
    entries.push_back(make_entry("c.png", 8, 8, cv::Scalar(70, 80, 90)));
    sel.insert(0);
    sel.insert(1);
    sel.insert(2);

    std::string err;
    svc.update(entries, sel, {}, err);
    REQUIRE(svc.size() == 2);
    REQUIRE(up.upload_count() == 2);
    REQUIRE(up.destroy_count() == 0);
    REQUIRE(up.live_count() == 2);

    // Recompute: the old textures must be destroyed before the new
    // ones are uploaded so the uploader's live set never grows past
    // the current slot count.
    svc.mark_dirty();
    svc.update(entries, sel, {}, err);
    REQUIRE(svc.size() == 2);
    REQUIRE(up.upload_count() == 4);
    REQUIRE(up.destroy_count() == 2);
    REQUIRE(up.live_count() == 2);
}

TEST_CASE("DiffService::clear destroys all textures and empties slots",
          "[diff_service]") {
    FakeUploader up;
    DiffService svc(up);
    SelectionModel sel;

    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("a.png", 8, 8, cv::Scalar(10, 20, 30)));
    entries.push_back(make_entry("b.png", 8, 8, cv::Scalar(40, 50, 60)));
    sel.insert(0);
    sel.insert(1);

    std::string err;
    svc.update(entries, sel, {}, err);
    REQUIRE(svc.size() == 1);
    REQUIRE(up.live_count() == 1);

    svc.clear();
    REQUIRE(svc.empty());
    REQUIRE(up.destroy_count() == 1);
    REQUIRE(up.live_count() == 0);
    // clear() must not flip the dirty flag; callers control that
    // explicitly.
    REQUIRE(!svc.is_dirty());
}

TEST_CASE("DiffService destructor releases all textures", "[diff_service]") {
    FakeUploader up;
    {
        DiffService svc(up);
        SelectionModel sel;

        std::vector<ImageEntry> entries;
        entries.push_back(make_entry("a.png", 8, 8, cv::Scalar(10, 20, 30)));
        entries.push_back(make_entry("b.png", 8, 8, cv::Scalar(40, 50, 60)));
        entries.push_back(make_entry("c.png", 8, 8, cv::Scalar(70, 80, 90)));
        sel.insert(0);
        sel.insert(1);
        sel.insert(2);

        std::string err;
        svc.update(entries, sel, {}, err);
        REQUIRE(svc.size() == 2);
        REQUIRE(up.live_count() == 2);
    }
    REQUIRE(up.live_count() == 0);
    REQUIRE(up.destroy_count() == 2);
}

TEST_CASE("DiffService::update is a no-op when not dirty", "[diff_service]") {
    FakeUploader up;
    DiffService svc(up);
    SelectionModel sel;

    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("a.png", 8, 8, cv::Scalar(10, 20, 30)));
    entries.push_back(make_entry("b.png", 8, 8, cv::Scalar(40, 50, 60)));
    sel.insert(0);
    sel.insert(1);

    std::string err;
    svc.update(entries, sel, {}, err);
    REQUIRE(svc.size() == 1);
    REQUIRE(up.upload_count() == 1);

    // Second call without mark_dirty(): the cache stays as-is and no
    // new uploads / destroys happen.
    svc.update(entries, sel, {}, err);
    REQUIRE(svc.size() == 1);
    REQUIRE(up.upload_count() == 1);
    REQUIRE(up.destroy_count() == 0);
}

TEST_CASE("DiffService skips partners with missing pixel data without "
          "blanking the rest", "[diff_service]") {
    FakeUploader up;
    DiffService svc(up);
    SelectionModel sel;

    std::vector<ImageEntry> entries;
    entries.push_back(make_entry("a.png", 8, 8, cv::Scalar(10, 20, 30)));
    // B has no Image attached: compute_difference can't run for this
    // partner.  The service must still attempt the next partner.
    entries.push_back(make_empty_entry("b.png"));
    entries.push_back(make_entry("c.png", 8, 8, cv::Scalar(70, 80, 90)));
    sel.insert(0);
    sel.insert(1);
    sel.insert(2);

    std::string err;
    svc.update(entries, sel, {}, err);

    // B is silently skipped (no error: the early-return for null
    // img_p does not emit a status string), but C is still produced.
    REQUIRE(svc.size() == 1);
    REQUIRE(svc.slots()[0].partner_entry_idx == 2);
    REQUIRE(up.upload_count() == 1);
}

TEST_CASE("DiffService aborts when A has no pixel data", "[diff_service]") {
    FakeUploader up;
    DiffService svc(up);
    SelectionModel sel;

    std::vector<ImageEntry> entries;
    entries.push_back(make_empty_entry("a.png"));
    entries.push_back(make_entry("b.png", 8, 8, cv::Scalar(40, 50, 60)));
    sel.insert(0);
    sel.insert(1);

    std::string err;
    svc.update(entries, sel, {}, err);

    // Without A there is no reference image; the whole update bails
    // and produces zero slots.
    REQUIRE(svc.empty());
    REQUIRE(up.upload_count() == 0);
}

TEST_CASE("DiffService::Options forwards heatmap_color choice", "[diff_service]") {
    // We don't try to assert specific RGB values here -- the colormap
    // semantics belong to image_comparator's tests.  This test only
    // verifies the service round-trips Options into the pipeline
    // (different colormaps must produce different heatmap pixels for
    // the same input).
    FakeUploader up;
    DiffService svc(up);
    SelectionModel sel;

    cv::Mat b_mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30));
    b_mat.at<cv::Vec3b>(0, 0) = cv::Vec3b(20, 30, 40);
    b_mat.at<cv::Vec3b>(1, 1) = cv::Vec3b(15, 25, 35);

    auto a_img = make_image(8, 8, cv::Scalar(10, 20, 30));
    auto b_img = std::make_unique<Image>();
    b_img->internal().mat = b_mat.clone();
    b_img->internal().info.width = 8;
    b_img->internal().info.height = 8;
    b_img->internal().info.pixel_format = PixelFormat::RGB8;
    b_img->internal().info.bit_depth = 8;

    std::vector<ImageEntry> entries;
    ImageEntry ea;
    ea.path = "/tmp/a.png";
    ea.filename = "a.png";
    ea.image = std::move(a_img);
    entries.push_back(std::move(ea));
    ImageEntry eb;
    eb.path = "/tmp/b.png";
    eb.filename = "b.png";
    eb.image = std::move(b_img);
    entries.push_back(std::move(eb));

    sel.insert(0);
    sel.insert(1);

    DiffService::Options opts_inferno;
    opts_inferno.heatmap_color = idiff::HeatmapColor::Inferno;
    std::string err;
    svc.update(entries, sel, opts_inferno, err);
    REQUIRE(svc.size() == 1);
    cv::Mat inferno_pixels = svc.slots()[0].image->mat().clone();

    DiffService::Options opts_gray;
    opts_gray.heatmap_color = idiff::HeatmapColor::Gray;
    svc.mark_dirty();
    svc.update(entries, sel, opts_gray, err);
    REQUIRE(svc.size() == 1);
    cv::Mat gray_pixels = svc.slots()[0].image->mat().clone();

    // Inferno is a fully chromatic colormap; Gray returns the raw
    // single-channel diff.  The two outputs differ both in shape and
    // in pixel content.
    cv::Mat diff;
    if (inferno_pixels.size() == gray_pixels.size() &&
        inferno_pixels.channels() == gray_pixels.channels()) {
        cv::absdiff(inferno_pixels, gray_pixels, diff);
        REQUIRE(cv::sum(diff)[0] > 0);
    }
    // It's enough that they aren't byte-identical buffers; either a
    // pixel-difference or a shape/channel mismatch satisfies the
    // contract.
}
