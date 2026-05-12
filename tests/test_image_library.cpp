// Unit tests for ImageLibrary.
//
// ImageLibrary is the "container-only" image collection service.  These
// tests cover:
//   * add / size / clear semantics
//   * remap correctness for remove, move, sort_with
//   * texture lifecycle: every entry created with a non-null fake
//     texture is destroyed exactly once when removed or when the
//     library is cleared / destroyed
//
// SDL_Texture* is treated as an opaque cookie: the FakeUploader hands
// out unique integer-cast pointers and records every destroy() call
// in order, so tests can assert the destruction set without touching
// SDL.

#include <catch2/catch_test_macros.hpp>

#include "domain/image_library.h"
#include "app/io/texture_uploader.h"
#include "app/app.h"
// Required so vector<ImageEntry>::erase / move-assign in test bodies
// can instantiate ImageEntry's destructor (which holds unique_ptr to
// the forward-declared Image / MediaSource types).
#include "core/image.h"        // IWYU pragma: keep
#include "core/media_source.h" // IWYU pragma: keep

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

class FakeUploader : public idiff::ITextureUploader {
public:
    SDL_Texture* upload(const idiff::UploadRequest& /*req*/) override {
        ++upload_count_;
        // Return a unique non-null cookie.  The library only treats
        // SDL_Texture* as an opaque handle, so any distinct pointer
        // serves as a fake texture.
        auto cookie = reinterpret_cast<SDL_Texture*>(
            static_cast<std::uintptr_t>(++next_cookie_));
        live_.insert(cookie);
        return cookie;
    }

    void destroy(SDL_Texture* tex) override {
        if (!tex) return;
        destroy_log_.push_back(tex);
        live_.erase(tex);
    }

    int upload_count() const { return upload_count_; }
    const std::vector<SDL_Texture*>& destroy_log() const { return destroy_log_; }
    const std::unordered_set<SDL_Texture*>& live() const { return live_; }

private:
    int upload_count_ = 0;
    std::uintptr_t next_cookie_ = 0;
    std::vector<SDL_Texture*> destroy_log_;
    std::unordered_set<SDL_Texture*> live_;
};

idiff::ImageEntry make_entry(const std::string& filename) {
    idiff::ImageEntry e;
    e.path = "/tmp/" + filename;
    e.filename = filename;
    e.display_label = filename;
    return e;
}

// Append four entries A B C D and pre-fill them with fake textures so
// remove/clear can exercise the destroy path.
void seed_abcd(idiff::ImageLibrary& lib, FakeUploader& up) {
    lib.add(make_entry("a.png"));
    lib.add(make_entry("b.png"));
    lib.add(make_entry("c.png"));
    lib.add(make_entry("d.png"));

    idiff::UploadRequest req;
    static const std::uint8_t pixel[4] = {0, 0, 0, 0};
    req.pixels = pixel;
    req.width = 1;
    req.height = 1;
    req.channels = 4;
    for (std::size_t i = 0; i < lib.size(); ++i) {
        lib.upload(i, req);
    }
    REQUIRE(up.upload_count() == 4);
}

} // namespace

TEST_CASE("ImageLibrary starts empty", "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);

    CHECK(lib.empty());
    CHECK(lib.size() == 0);
    CHECK(lib.all().empty());
}

TEST_CASE("ImageLibrary add appends in insertion order", "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);

    lib.add(make_entry("first.png"));
    lib.add(make_entry("second.png"));

    REQUIRE(lib.size() == 2);
    CHECK(lib.at(0).filename == "first.png");
    CHECK(lib.at(1).filename == "second.png");
}

TEST_CASE("ImageLibrary upload binds texture and clears dirty flag",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    lib.add(make_entry("a.png"));

    idiff::UploadRequest req;
    static const std::uint8_t pixel[4] = {0, 0, 0, 0};
    req.pixels = pixel;
    req.width = 32;
    req.height = 16;
    req.channels = 4;
    lib.upload(0, req);

    REQUIRE(lib.at(0).texture != nullptr);
    CHECK(lib.at(0).tex_w == 32);
    CHECK(lib.at(0).tex_h == 16);
    CHECK(lib.at(0).texture_dirty == false);
}

TEST_CASE("ImageLibrary upload replaces existing texture and destroys the old one",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    lib.add(make_entry("a.png"));

    idiff::UploadRequest req;
    static const std::uint8_t pixel[4] = {0, 0, 0, 0};
    req.pixels = pixel;
    req.width = 1;
    req.height = 1;
    req.channels = 4;

    lib.upload(0, req);
    auto* first = lib.at(0).texture;
    REQUIRE(first != nullptr);

    lib.upload(0, req);
    auto* second = lib.at(0).texture;

    CHECK(second != nullptr);
    CHECK(second != first);
    // The first texture must be destroyed exactly once.
    CHECK(up.destroy_log().size() == 1);
    CHECK(up.destroy_log().front() == first);
}

TEST_CASE("ImageLibrary upload with out-of-range index is a no-op",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    lib.add(make_entry("a.png"));

    idiff::UploadRequest req;
    static const std::uint8_t pixel[4] = {0, 0, 0, 0};
    req.pixels = pixel;
    req.width = 1;
    req.height = 1;
    req.channels = 4;

    lib.upload(5, req);

    CHECK(up.upload_count() == 0);
    CHECK(lib.at(0).texture == nullptr);
}

TEST_CASE("ImageLibrary remove erases entry, destroys texture, returns shifting remap",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    seed_abcd(lib, up);

    auto* removed_tex = lib.at(1).texture;
    REQUIRE(removed_tex != nullptr);

    auto remap = lib.remove(1);

    REQUIRE(remap.size() == 4);
    CHECK(remap[0] == 0);
    CHECK(remap[1] == idiff::ImageLibrary::kRemoved);
    CHECK(remap[2] == 1);
    CHECK(remap[3] == 2);

    REQUIRE(lib.size() == 3);
    CHECK(lib.at(0).filename == "a.png");
    CHECK(lib.at(1).filename == "c.png");
    CHECK(lib.at(2).filename == "d.png");

    // The removed entry's texture must have been destroyed exactly once.
    int count = 0;
    for (auto* t : up.destroy_log()) {
        if (t == removed_tex) ++count;
    }
    CHECK(count == 1);
}

TEST_CASE("ImageLibrary remove out-of-range returns empty remap",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    seed_abcd(lib, up);

    auto remap = lib.remove(99);
    CHECK(remap.empty());
    CHECK(lib.size() == 4);
    CHECK(up.destroy_log().empty());
}

TEST_CASE("ImageLibrary move forward shifts intermediates left",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    seed_abcd(lib, up);
    // Start order: A B C D.  move(0, 2) -> B C A D.
    auto remap = lib.move(0, 2);

    REQUIRE(lib.size() == 4);
    CHECK(lib.at(0).filename == "b.png");
    CHECK(lib.at(1).filename == "c.png");
    CHECK(lib.at(2).filename == "a.png");
    CHECK(lib.at(3).filename == "d.png");

    REQUIRE(remap.size() == 4);
    CHECK(remap[0] == 2);  // A: 0 -> 2
    CHECK(remap[1] == 0);  // B: 1 -> 0
    CHECK(remap[2] == 1);  // C: 2 -> 1
    CHECK(remap[3] == 3);  // D unchanged
}

TEST_CASE("ImageLibrary move backward shifts intermediates right",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    seed_abcd(lib, up);
    // Start order: A B C D.  move(3, 1) -> A D B C.
    auto remap = lib.move(3, 1);

    CHECK(lib.at(0).filename == "a.png");
    CHECK(lib.at(1).filename == "d.png");
    CHECK(lib.at(2).filename == "b.png");
    CHECK(lib.at(3).filename == "c.png");

    REQUIRE(remap.size() == 4);
    CHECK(remap[0] == 0);
    CHECK(remap[1] == 2);  // B: 1 -> 2
    CHECK(remap[2] == 3);  // C: 2 -> 3
    CHECK(remap[3] == 1);  // D: 3 -> 1
}

TEST_CASE("ImageLibrary move with from == to returns empty remap and changes nothing",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    seed_abcd(lib, up);

    auto remap = lib.move(2, 2);
    CHECK(remap.empty());
    CHECK(lib.at(2).filename == "c.png");
}

TEST_CASE("ImageLibrary sort_by_filename orders entries case-insensitively",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    lib.add(make_entry("Charlie.png"));
    lib.add(make_entry("alpha.png"));
    lib.add(make_entry("Bravo.png"));

    auto remap = lib.sort_by_filename();

    REQUIRE(lib.size() == 3);
    CHECK(lib.at(0).filename == "alpha.png");
    CHECK(lib.at(1).filename == "Bravo.png");
    CHECK(lib.at(2).filename == "Charlie.png");

    REQUIRE(remap.size() == 3);
    CHECK(remap[0] == 2);  // Charlie original index 0 -> 2
    CHECK(remap[1] == 0);  // alpha   original index 1 -> 0
    CHECK(remap[2] == 1);  // Bravo   original index 2 -> 1
}

TEST_CASE("ImageLibrary sort_with respects custom comparator",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    lib.add(make_entry("a.png"));
    lib.add(make_entry("b.png"));
    lib.add(make_entry("c.png"));

    // Reverse ASCII order.
    auto remap = lib.sort_with([](const std::string& a, const std::string& b) {
        return a > b;
    });

    CHECK(lib.at(0).filename == "c.png");
    CHECK(lib.at(1).filename == "b.png");
    CHECK(lib.at(2).filename == "a.png");

    REQUIRE(remap.size() == 3);
    CHECK(remap[0] == 2);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 0);
}

TEST_CASE("ImageLibrary sort_by_filename on empty/single returns identity",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);

    auto remap0 = lib.sort_by_filename();
    CHECK(remap0.empty());

    lib.add(make_entry("only.png"));
    auto remap1 = lib.sort_by_filename();
    REQUIRE(remap1.size() == 1);
    CHECK(remap1[0] == 0);
}

TEST_CASE("ImageLibrary clear destroys every texture and empties the list",
          "[domain][image_library]") {
    FakeUploader up;
    idiff::ImageLibrary lib(up);
    seed_abcd(lib, up);

    REQUIRE(up.live().size() == 4);

    lib.clear();

    CHECK(lib.empty());
    CHECK(up.live().empty());
    CHECK(up.destroy_log().size() == 4);
}

TEST_CASE("ImageLibrary destructor releases remaining textures",
          "[domain][image_library]") {
    FakeUploader up;
    {
        idiff::ImageLibrary lib(up);
        seed_abcd(lib, up);
    }
    CHECK(up.live().empty());
    CHECK(up.destroy_log().size() == 4);
}
