// Unit tests for ComparisonConfigService.
//
// The service parses a comparison-config JSON file, owns a per-config
// UrlCache, and resolves group switches into "local paths to load + how
// to label them".  These tests drive it end-to-end without ever
// invoking curl: every URL is pre-staged at the path UrlCache::path_for
// will resolve to, so fetch() always takes its fast (file-already-on-
// disk) path.
//
// Tests use file:// URLs so we don't depend on network connectivity.
//
// Coverage:
//   * load() rejects malformed JSON with ok=false and leaves state empty
//   * load() accepts a valid config and reports group/item counts
//   * switch_to() rejects out-of-range and no-op-when-already-current
//   * switch_to() returns ComparisonGroupEntry per fetched URL,
//     populated with display_label from item.title or URL basename
//   * switch_to() reports failures and last_error when an item is
//     missing on disk
//   * clear() drops config and url cache (has_config -> false)

#include <catch2/catch_test_macros.hpp>

#include "domain/comparison_config_service.h"
#include "core/url_cache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using idiff::ComparisonConfigService;

namespace {

// Per-test scratch directory under the OS temp area.  Removed on
// destruction so back-to-back runs don't accumulate cache dirs.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 0x7fffffff);
        for (int i = 0; i < 16; ++i) {
            auto candidate = fs::temp_directory_path() /
                ("idiff_ccs_test_" + std::to_string(dist(gen)));
            std::error_code ec;
            if (fs::create_directories(candidate, ec)) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create temp directory");
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write_text(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

// Stage a non-empty file at a URL's predicted cache path so
// UrlCache::fetch() takes the fast path and never spawns curl.
void stage_url(const idiff::UrlCache& cache, const std::string& url) {
    auto target = cache.path_for(url);
    write_text(target, "fake image bytes");
}

} // namespace

TEST_CASE("load: malformed JSON is rejected", "[comparison_config_service]") {
    TempDir tmp;
    auto json_path = tmp.path() / "broken.json";
    write_text(json_path, "this is not JSON at all");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path());
    auto result = svc.load(json_path.string());

    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.status_message.empty());
    REQUIRE_FALSE(svc.has_config());
    REQUIRE(svc.group_count() == 0);
    REQUIRE(svc.current_index() == -1);
}

TEST_CASE("load: valid config reports counts and stays at index -1",
          "[comparison_config_service]") {
    TempDir tmp;
    auto json_path = tmp.path() / "ok.json";
    write_text(json_path, R"({
        "groups": [
            {"name": "g1", "items": [
                {"url": "file:///a.png", "title": "A"},
                {"url": "file:///b.png"}
            ]},
            {"name": "g2", "items": [
                {"url": "file:///c.png"}
            ]}
        ]
    })");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path());
    auto result = svc.load(json_path.string());

    REQUIRE(result.ok);
    REQUIRE(svc.has_config());
    REQUIRE(svc.group_count() == 2);
    REQUIRE(svc.current_index() == -1);
    REQUIRE(svc.config().groups[0].name == "g1");
    REQUIRE(svc.config().groups[0].items.size() == 2);
    REQUIRE(svc.config().groups[1].items.size() == 1);
    REQUIRE(result.status_message.find("2 group") != std::string::npos);
    REQUIRE(result.status_message.find("3 image") != std::string::npos);
}

TEST_CASE("switch_to: out-of-range index is rejected",
          "[comparison_config_service]") {
    TempDir tmp;
    auto json_path = tmp.path() / "ok.json";
    write_text(json_path, R"({"groups": [{"name": "g1", "items": [
        {"url": "file:///a.png"}
    ]}]})");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path());
    REQUIRE(svc.load(json_path.string()).ok);

    auto neg = svc.switch_to(-1);
    REQUIRE_FALSE(neg.ok);
    REQUIRE(svc.current_index() == -1);

    auto past = svc.switch_to(99);
    REQUIRE_FALSE(past.ok);
    REQUIRE(svc.current_index() == -1);
}

TEST_CASE("switch_to: no-op when index already current",
          "[comparison_config_service]") {
    TempDir tmp;
    auto json_path = tmp.path() / "ok.json";
    write_text(json_path, R"({"groups": [{"name": "g1", "items": [
        {"url": "file:///a.png"}
    ]}]})");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path());
    REQUIRE(svc.load(json_path.string()).ok);
    stage_url(*svc.url_cache_for_test(), "file:///a.png");

    auto first = svc.switch_to(0);
    REQUIRE(first.ok);
    REQUIRE(svc.current_index() == 0);

    auto second = svc.switch_to(0);
    REQUIRE_FALSE(second.ok);  // no-op, not an error per se
    REQUIRE(second.entries.empty());
    REQUIRE(svc.current_index() == 0);
}

TEST_CASE("switch_to: returns local paths and labels for fetched items",
          "[comparison_config_service]") {
    TempDir tmp;
    auto json_path = tmp.path() / "ok.json";
    write_text(json_path, R"({"groups": [{"name": "g1", "items": [
        {"url": "file:///dir/a.png", "title": "Image A"},
        {"url": "file:///dir/b.png"}
    ]}]})");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path());
    REQUIRE(svc.load(json_path.string()).ok);

    auto* cache = svc.url_cache_for_test();
    REQUIRE(cache != nullptr);
    stage_url(*cache, "file:///dir/a.png");
    stage_url(*cache, "file:///dir/b.png");

    auto result = svc.switch_to(0);

    REQUIRE(result.ok);
    REQUIRE(result.requested == 2);
    REQUIRE(result.failures == 0);
    REQUIRE(result.entries.size() == 2);
    REQUIRE(svc.current_index() == 0);

    // Per-item label: explicit title wins; absent title falls back to
    // the URL basename ("b.png") instead of being left empty.
    bool saw_title = false;
    bool saw_basename = false;
    for (const auto& e : result.entries) {
        REQUIRE_FALSE(e.local_path.empty());
        if (e.display_label == "Image A") saw_title = true;
        if (e.display_label == "b.png") saw_basename = true;
    }
    REQUIRE(saw_title);
    REQUIRE(saw_basename);
}

TEST_CASE("switch_to: missing items are reported as failures",
          "[comparison_config_service]") {
    TempDir tmp;
    auto json_path = tmp.path() / "ok.json";
    write_text(json_path, R"({"groups": [{"name": "g1", "items": [
        {"url": "file:///dir/present.png"},
        {"url": "file:///dir/missing.png"}
    ]}]})");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path());
    REQUIRE(svc.load(json_path.string()).ok);
    stage_url(*svc.url_cache_for_test(), "file:///dir/present.png");
    // Intentionally do NOT stage missing.png so curl fails.

    auto result = svc.switch_to(0);

    REQUIRE(result.requested == 2);
    REQUIRE(result.failures == 1);
    REQUIRE(result.entries.size() == 1);
    REQUIRE_FALSE(result.last_error.empty());
}

TEST_CASE("load: resolves images from local JSON dir (no download)",
          "[comparison_config_service]") {
    // Mirrors the real layout: the JSON lives one level inside the tree
    // that mirrors the URL path (the URL carries an extra "sr/" segment
    // matching the JSON's own folder name).  A local copy must be found
    // by walking up to an ancestor of the JSON directory, and no curl /
    // cache directory is involved.
    TempDir tmp;
    auto json_dir = tmp.path() / "sr";
    auto json_path = json_dir / "compare.json";
    // Local image sits under json_dir (the extra "sr/" in the URL is the
    // JSON's own folder name).  The ancestor base (tmp) + URL suffix
    // "sr/src/g1/a.png" resolves to tmp/sr/src/g1/a.png == json_dir/src/g1/a.png.
    auto local_image = json_dir / "src" / "g1" / "a.png";
    write_text(json_path, R"({"groups": [{"name": "g1", "items": [
        {"url": "https://host.example.com/sr/src/g1/a.png", "title": "A"}
    ]}]})");
    write_text(local_image, "real image bytes");

    ComparisonConfigService svc;
    // No cache-root override: load() should default to the JSON's own
    // directory and resolve the local file, not create an idiff_cache_*
    // directory under Downloads.
    REQUIRE(svc.load(json_path.string()).ok);
    REQUIRE(svc.has_config());

    auto result = svc.switch_to(0);

    REQUIRE(result.ok);
    REQUIRE(result.requested == 1);
    REQUIRE(result.failures == 0);
    REQUIRE(result.entries.size() == 1);
    REQUIRE_FALSE(result.entries.empty());
    // The returned local path must be the on-disk file, not a download
    // cache path.
    REQUIRE(result.entries[0].local_path == local_image.string());
    REQUIRE(result.entries[0].display_label == "A");
}

TEST_CASE("clear: drops config and cache", "[comparison_config_service]") {
    TempDir tmp;
    auto json_path = tmp.path() / "ok.json";
    write_text(json_path, R"({"groups": [{"name": "g", "items": [
        {"url": "file:///a.png"}
    ]}]})");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path());
    REQUIRE(svc.load(json_path.string()).ok);
    REQUIRE(svc.has_config());
    REQUIRE(svc.url_cache_for_test() != nullptr);

    svc.clear();

    REQUIRE_FALSE(svc.has_config());
    REQUIRE(svc.group_count() == 0);
    REQUIRE(svc.current_index() == -1);
    REQUIRE(svc.url_cache_for_test() == nullptr);
}

TEST_CASE("load: absent local file falls through to cache path",
          "[comparison_config_service]") {
    // When no local file matches the URL, path_for() must return a
    // cache-root-based path so the caller can download into it.
    TempDir tmp;
    auto json_path = tmp.path() / "compare.json";
    write_text(json_path, R"({"groups": [{"name": "g", "items": [
        {"url": "https://host.example.com/img/x.png", "title": "X"}
    ]}]})");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path() / "cache");
    REQUIRE(svc.load(json_path.string()).ok);

    // The image does not exist locally, so path_for() should return
    // a path under the cache root, not under json_dir.
    auto* cache = svc.url_cache_for_test();
    REQUIRE(cache);
    auto resolved = cache->path_for("https://host.example.com/img/x.png");
    // The resolved path must be under the cache root, not under
    // json_dir's parent (the ancestor-walk base).
    REQUIRE(resolved.string().find("cache") != std::string::npos);
}

TEST_CASE("load: empty local base disables local resolution",
          "[comparison_config_service]") {
    // After set_local_base(""), local_candidate must return empty so
    // path_for() falls through to the download path.
    TempDir tmp;
    auto json_path = tmp.path() / "compare.json";
    auto local_image = tmp.path() / "img" / "y.png";
    write_text(json_path, R"({"groups": [{"name": "g", "items": [
        {"url": "https://host.example.com/img/y.png", "title": "Y"}
    ]}]})");
    write_text(local_image, "bytes");

    ComparisonConfigService svc;
    svc.set_cache_root_override(tmp.path() / "cache");
    REQUIRE(svc.load(json_path.string()).ok);

    auto* cache = svc.url_cache_for_test();
    REQUIRE(cache);
    cache->set_local_base("");  // disable local resolution

    auto resolved = cache->path_for("https://host.example.com/img/y.png");
    // Must NOT be the local_image path; should be under cache root.
    REQUIRE(resolved != local_image);
    REQUIRE(resolved.string().find("cache") != std::string::npos);
}
