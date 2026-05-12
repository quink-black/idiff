#include "app/io/texture_uploader.h"
#include "app/io/file_dialog.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using idiff::FileDialogFilter;
using idiff::FileDialogResult;
using idiff::IFileDialog;
using idiff::SdlTextureUploader;
using idiff::UploadRequest;

namespace {

class FakeFileDialog : public IFileDialog {
public:
    enum class Op { OpenMultiple, OpenSingle, Save };

    struct Call {
        Op op;
        std::vector<FileDialogFilter> filters;
        std::string default_name;
    };

    std::vector<Call> calls;
    FileDialogResult next;

    FileDialogResult open_multiple(
        const std::vector<FileDialogFilter>& filters) override {
        calls.push_back({Op::OpenMultiple, filters, {}});
        return next;
    }
    FileDialogResult open_single(
        const std::vector<FileDialogFilter>& filters) override {
        calls.push_back({Op::OpenSingle, filters, {}});
        return next;
    }
    FileDialogResult save(
        const std::vector<FileDialogFilter>& filters,
        std::string_view default_name) override {
        calls.push_back({Op::Save, filters, std::string(default_name)});
        return next;
    }
};

} // namespace

TEST_CASE("SdlTextureUploader rejects bad inputs without crashing", "[io]") {
    // We deliberately pass nullptr for the renderer.  A real upload
    // would dereference it; the validation guard must short-circuit
    // before ever calling SDL.
    SdlTextureUploader up(nullptr);

    UploadRequest empty;
    REQUIRE(up.upload(empty) == nullptr);

    std::vector<std::uint8_t> pixels(4 * 4 * 4, 0xff);
    UploadRequest req;
    req.pixels = pixels.data();
    req.width = 4;
    req.height = 4;
    req.channels = 3; // not RGBA -> rejected
    REQUIRE(up.upload(req) == nullptr);

    req.channels = 4;
    req.width = 0; // bad geometry -> rejected
    REQUIRE(up.upload(req) == nullptr);

    // Destroying nullptr is a no-op.
    up.destroy(nullptr);
}

TEST_CASE("FakeFileDialog records what was asked of it", "[io]") {
    FakeFileDialog fake;

    fake.next = FileDialogResult{};
    fake.next.paths = {"/tmp/a.png", "/tmp/b.png"};

    std::vector<FileDialogFilter> filters = {
        {"PNG", "png"},
        {"JPEG", "jpg,jpeg"},
    };
    auto r = fake.open_multiple(filters);
    REQUIRE(r.paths.size() == 2);
    REQUIRE(r.error.empty());
    REQUIRE(fake.calls.size() == 1);
    REQUIRE(fake.calls[0].op == FakeFileDialog::Op::OpenMultiple);
    REQUIRE(fake.calls[0].filters.size() == 2);
    REQUIRE(fake.calls[0].filters[0].label == std::string_view("PNG"));

    fake.next = FileDialogResult{};
    fake.next.error = "permission denied";
    auto r2 = fake.open_single(filters);
    REQUIRE(r2.paths.empty());
    REQUIRE(r2.error == "permission denied");
    REQUIRE(fake.calls.back().op == FakeFileDialog::Op::OpenSingle);

    fake.next = FileDialogResult{}; // cancelled
    auto r3 = fake.save(filters, "viewport.png");
    REQUIRE(r3.paths.empty());
    REQUIRE(r3.error.empty());
    REQUIRE(fake.calls.back().op == FakeFileDialog::Op::Save);
    REQUIRE(fake.calls.back().default_name == "viewport.png");
}
