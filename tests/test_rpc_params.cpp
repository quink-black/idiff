// Entry references accepted by the RPC handlers: an integer index, or a
// string matched against ImageEntry::path -- verbatim first, then as
// the same file on disk -- either bare (resolve_entry), in an array
// (resolve_entries), or as one of the "index" / "path" fields
// (require_entry_field).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "app/app.h"             // ImageEntry
#include "app/rpc/rpc_dispatcher.h"
#include "app/rpc_params.h"
#include "core/media_source.h"   // complete type for ImageEntry's unique_ptr

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using idiff::rpc::ErrorCode;
using idiff::rpc::RpcException;
using nlohmann::json;
using namespace idiff::rpc_params;

namespace {

std::vector<idiff::ImageEntry> make_entries(
        const std::vector<std::string>& paths) {
    std::vector<idiff::ImageEntry> out;
    for (const auto& p : paths) {
        idiff::ImageEntry e;
        e.path = p;
        out.push_back(std::move(e));
    }
    return out;
}

// Runs `fn`, requires it to throw InvalidParams, and returns the message.
template <typename Fn>
std::string invalid_params_message(Fn&& fn) {
    try {
        fn();
    } catch (const RpcException& ex) {
        REQUIRE(ex.code() == ErrorCode::InvalidParams);
        return ex.msg();
    }
    FAIL("expected RpcException");
    return {};
}

// A directory holding one empty file, removed again on destruction.
class TempFile {
public:
    TempFile() {
        namespace fs = std::filesystem;
        dir_ = fs::temp_directory_path() /
               ("idiff-rpc-params-" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_ / "sub");
        file_ = dir_ / "a.png";
        std::ofstream(file_).put('x');
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::filesystem::path& dir() const { return dir_; }
    const std::filesystem::path& file() const { return file_; }

private:
    std::filesystem::path dir_;
    std::filesystem::path file_;
};

} // namespace

TEST_CASE("resolve_entry accepts an in-range index", "[rpc][params]") {
    auto entries = make_entries({"/a.png", "/b.png"});
    REQUIRE(resolve_entry(json(1), entries, "ref") == 1);
}

TEST_CASE("resolve_entry rejects an out-of-range index", "[rpc][params]") {
    auto entries = make_entries({"/a.png"});
    auto msg = invalid_params_message(
        [&] { resolve_entry(json(1), entries, "ref"); });
    REQUIRE_THAT(msg, Catch::Matchers::ContainsSubstring("ref out of range"));
    invalid_params_message([&] { resolve_entry(json(-1), entries, "ref"); });
}

TEST_CASE("resolve_entry finds the entry with a unique path",
          "[rpc][params]") {
    auto entries = make_entries({"/a.png", "/b.png", "/c.png"});
    REQUIRE(resolve_entry(json("/c.png"), entries, "ref") == 2);
}

TEST_CASE("resolve_entry rejects a path no entry has", "[rpc][params]") {
    auto entries = make_entries({"/a.png"});
    auto msg = invalid_params_message(
        [&] { resolve_entry(json("/missing.png"), entries, "ref"); });
    REQUIRE_THAT(msg, Catch::Matchers::ContainsSubstring("/missing.png"));
}

TEST_CASE("resolve_entry rejects a path several entries share and lists them",
          "[rpc][params]") {
    auto entries = make_entries({"/a.png", "/b.png", "/a.png"});
    auto msg = invalid_params_message(
        [&] { resolve_entry(json("/a.png"), entries, "ref"); });
    REQUIRE_THAT(msg, Catch::Matchers::ContainsSubstring("0, 2"));
}

TEST_CASE("resolve_entry matches a path that names the same file",
          "[rpc][params]") {
    TempFile tmp;
    auto entries = make_entries({"/elsewhere.png", tmp.file().string()});

    const auto dotted = tmp.dir() / "sub" / ".." / "a.png";
    REQUIRE(resolve_entry(json(dotted.string()), entries, "ref") == 1);

    const auto link = tmp.dir() / "link.png";
    std::error_code ec;
    std::filesystem::create_symlink(tmp.file(), link, ec);
    if (!ec) {
        REQUIRE(resolve_entry(json(link.string()), entries, "ref") == 1);
    }

    invalid_params_message([&] {
        resolve_entry(json((tmp.dir() / "b.png").string()), entries, "ref");
    });
}

TEST_CASE("resolve_entry prefers a verbatim match over the same file",
          "[rpc][params]") {
    TempFile tmp;
    const auto dotted = (tmp.dir() / "sub" / ".." / "a.png").string();
    auto entries = make_entries({tmp.file().string(), dotted});
    REQUIRE(resolve_entry(json(dotted), entries, "ref") == 1);
}

TEST_CASE("resolve_entry rejects references that are neither int nor string",
          "[rpc][params]") {
    auto entries = make_entries({"/a.png"});
    invalid_params_message([&] { resolve_entry(json(0.5), entries, "ref"); });
    invalid_params_message([&] { resolve_entry(json(true), entries, "ref"); });
    invalid_params_message([&] { resolve_entry(json(nullptr), entries, "ref"); });
}

TEST_CASE("resolve_entries resolves a mixed array in order", "[rpc][params]") {
    auto entries = make_entries({"/a.png", "/b.png", "/c.png"});
    auto got = resolve_entries(json::array({"/c.png", 0, "/b.png"}),
                               entries, "targets");
    REQUIRE(got == std::vector<int>{2, 0, 1});
}

TEST_CASE("resolve_entries rejects a non-array and names the bad element",
          "[rpc][params]") {
    auto entries = make_entries({"/a.png"});
    invalid_params_message(
        [&] { resolve_entries(json("/a.png"), entries, "targets"); });
    auto msg = invalid_params_message([&] {
        resolve_entries(json::array({0, "/nope.png"}), entries, "targets");
    });
    REQUIRE_THAT(msg, Catch::Matchers::ContainsSubstring("targets[]"));
}

TEST_CASE("require_entry_field takes index or path", "[rpc][params]") {
    auto entries = make_entries({"/a.png", "/b.png"});
    REQUIRE(require_entry_field(json{{"index", 1}}, entries) == 1);
    REQUIRE(require_entry_field(json{{"path", "/a.png"}}, entries) == 0);
}

TEST_CASE("require_entry_field requires exactly one of index and path",
          "[rpc][params]") {
    auto entries = make_entries({"/a.png"});
    invalid_params_message(
        [&] { require_entry_field(json::object(), entries); });
    invalid_params_message([&] {
        require_entry_field(json{{"index", 0}, {"path", "/a.png"}}, entries);
    });
}

TEST_CASE("require_entry_field checks the type of each field",
          "[rpc][params]") {
    auto entries = make_entries({"/a.png"});
    invalid_params_message(
        [&] { require_entry_field(json{{"index", "/a.png"}}, entries); });
    invalid_params_message(
        [&] { require_entry_field(json{{"path", 0}}, entries); });
}
