#include "core/url_cache.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace idiff {
namespace {

// ---------------------------------------------------------------------------
// URL parsing helpers.  We intentionally keep this dependency-free; the
// comparison-config workflow only ever feeds us `http(s)://...`, `file://`,
// or bare paths, all of which are easy to split by hand.
// ---------------------------------------------------------------------------

// URL-decode (percent-escape) a single path segment.  Unknown escapes are
// left untouched so a malformed URL just yields a slightly ugly filename
// rather than a crash.
std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// Replace characters that are problematic on common filesystems (Windows
// especially) with underscores.  We deliberately preserve '.', '-', and
// '_' so filenames like "kid-pisa-v0.jpg" survive intact.
std::string sanitize_segment(std::string_view s) {
    static constexpr std::string_view kBad = "<>:\"\\|?*";
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) { out.push_back('_'); continue; }
        if (kBad.find(c) != std::string_view::npos) {
            out.push_back('_');
            continue;
        }
        out.push_back(c);
    }
    // Avoid empty and dot-only names (which confuse std::filesystem).
    if (out.empty() || out == "." || out == "..") out = "_";
    return out;
}

struct ParsedUrl {
    std::string host;           // may be empty (e.g. file:// or bare path)
    std::string path_no_query;  // leading '/' stripped; "" when only a host
};

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl p;
    std::string_view v(url);

    // Strip scheme, if any.
    if (auto pos = v.find("://"); pos != std::string_view::npos) {
        v.remove_prefix(pos + 3);
    }
    // Drop query / fragment.
    if (auto q = v.find_first_of("?#"); q != std::string_view::npos) {
        v.remove_suffix(v.size() - q);
    }
    // Split host from path at the first '/' after the authority.
    auto slash = v.find('/');
    if (slash == std::string_view::npos) {
        p.host = std::string(v);
    } else {
        p.host = std::string(v.substr(0, slash));
        p.path_no_query = std::string(v.substr(slash + 1));  // strip leading '/'
    }
    return p;
}

// Portable shell-quoting.  Only destination paths need this -- URLs are
// passed through unmodified after basic validation further up the stack.
std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
#endif
}

// Expand a leading "~/" using $HOME (POSIX) or %USERPROFILE% (Windows).
// Non-tilde paths are returned verbatim.
std::filesystem::path expand_tilde(const std::string& raw) {
    if (raw.empty() || raw[0] != '~') return std::filesystem::path(raw);
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || !*home) return std::filesystem::path(raw);
    if (raw.size() == 1) return std::filesystem::path(home);
    if (raw[1] == '/' || raw[1] == '\\') {
        return std::filesystem::path(home) / raw.substr(2);
    }
    return std::filesystem::path(raw);
}

// Trim ASCII whitespace (and surrounding quotes) from both ends of `s`.
void trim_inplace(std::string& s) {
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    if (s.size() >= 2 &&
        ((s.front() == '"'  && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// UrlCache public API
// ---------------------------------------------------------------------------

UrlCache::UrlCache() : root_(resolve_default_root()) {}
UrlCache::UrlCache(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path UrlCache::read_user_config_root() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || !*home) return {};
    fs::path cfg = fs::path(home) / ".idiff.config";

    std::error_code ec;
    if (!fs::exists(cfg, ec) || ec) return {};

    std::ifstream in(cfg);
    if (!in.is_open()) return {};

    // We accept both `cache_root = /path` style entries and a bare path on
    // its own line so the config file is friendly to hand-editing.  First
    // non-empty, non-comment line wins.
    std::string line;
    while (std::getline(in, line)) {
        // Strip comments (#...).
        if (auto h = line.find('#'); h != std::string::npos) {
            line.erase(h);
        }
        trim_inplace(line);
        if (line.empty()) continue;

        std::string value;
        if (auto eq = line.find('='); eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            trim_inplace(key);
            for (auto& c : key) {
                c = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(c)));
            }
            if (key != "cache_root" && key != "cacheroot" &&
                key != "cache-dir"  && key != "cache_dir") {
                continue;  // unknown key, try next line
            }
            value = line.substr(eq + 1);
        } else {
            // Bare path form.
            value = line;
        }
        trim_inplace(value);
        if (value.empty()) continue;
        return expand_tilde(value);
    }
    return {};
}

std::filesystem::path UrlCache::downloads_dir() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) {
        return fs::path(profile) / "Downloads";
    }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / "Downloads";
    }
#endif
    return {};
}

std::filesystem::path UrlCache::resolve_default_root() {
    if (auto configured = read_user_config_root(); !configured.empty()) {
        return configured;
    }
    if (auto dl = downloads_dir(); !dl.empty()) {
        return dl;
    }
    return std::filesystem::path(".");
}

std::string UrlCache::make_config_cache_dirname(
    const std::filesystem::path& json_file) {
    // Use the stem so "foo.json" -> "foo", and sanitize to keep the
    // resulting directory name portable.
    std::string stem = json_file.stem().string();
    stem = sanitize_segment(stem);
    if (stem.empty()) stem = "config";

    // Local-time timestamp, fixed at call time so all URLs from one
    // config land in the same directory even across many fetches.
    using clock = std::chrono::system_clock;
    auto now = clock::to_time_t(clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);

    return "idiff_cache_" + stem + "_" + buf;
}

std::filesystem::path UrlCache::path_for(const std::string& url) const {
    namespace fs = std::filesystem;
    ParsedUrl p = parse_url(url);

    fs::path result = root_;

    // Host becomes the first level below the cache root so assets from
    // different sites are obviously separated.  Missing host (file://,
    // bare path) falls back to a stable "_local" folder.  When
    // register_urls() observed that every URL in this session shares
    // the same host, we drop it entirely: the cache root is already
    // per-config (resolve_default_root() / make_config_cache_dirname()),
    // so the host directory would just add visual noise.
    std::string host = sanitize_segment(url_decode(p.host));
    if (host.empty()) host = "_local";
    const bool drop_host = (!strip_host_.empty() && host == strip_host_);
    if (!drop_host) {
        result /= host;
    }

    // Preserve the URL's path structure one segment at a time, each
    // sanitized independently so '/' is never swallowed into a
    // filename.  An empty path (e.g. "https://example.com/") yields
    // "index" so we still have a concrete file to write.
    if (p.path_no_query.empty()) {
        result /= "index";
    } else {
        // First, collect all sanitized segments so we can honour the
        // register_urls()-computed "strip N leading segments" setting
        // in one place.  Keeping the original split logic here (so the
        // trailing-slash "index" leaf still works) would double up the
        // bookkeeping; gathering into a vector is clearer.
        std::vector<std::string> segs;
        segs.reserve(8);
        std::size_t start = 0;
        while (start <= p.path_no_query.size()) {
            std::size_t slash = p.path_no_query.find('/', start);
            std::size_t end = (slash == std::string::npos)
                                  ? p.path_no_query.size()
                                  : slash;
            std::string seg(p.path_no_query.data() + start, end - start);
            if (!seg.empty()) {
                segs.push_back(sanitize_segment(url_decode(seg)));
            }
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        // Only drop common prefix segments when this URL actually
        // shares them -- URLs outside the registered set might not,
        // and we must not strip past the last segment (otherwise
        // different URLs could collapse onto the same local path).
        std::size_t skip = 0;
        if (drop_host && !segs.empty() && strip_segments_ > 0) {
            skip = std::min<std::size_t>(strip_segments_,
                                         segs.size() - 1);
        }
        for (std::size_t i = skip; i < segs.size(); ++i) {
            result /= segs[i];
        }
        // If the URL ended in '/', append an "index" leaf so we still
        // materialize a file and not a directory.
        if (!p.path_no_query.empty() && p.path_no_query.back() == '/') {
            result /= "index";
        }
    }
    return result;
}

void UrlCache::register_urls(const std::vector<std::string>& urls) {
    // Reset to a no-op trim first.  Any early return below then
    // correctly falls back to the "mirror the URL verbatim" behaviour.
    strip_host_.clear();
    strip_segments_ = 0;
    if (urls.empty()) return;

    // Decompose every URL into (sanitized host, sanitized segments).
    // The sanitize step is the same one path_for() applies, so the
    // "strip N segments" count we compute here lines up 1:1 with
    // what the path composition loop sees.
    struct Decomposed {
        std::string host;
        std::vector<std::string> segs;
    };
    std::vector<Decomposed> decoded;
    decoded.reserve(urls.size());
    for (const auto& u : urls) {
        ParsedUrl p = parse_url(u);
        Decomposed d;
        d.host = sanitize_segment(url_decode(p.host));
        if (d.host.empty()) d.host = "_local";

        std::size_t start = 0;
        while (start <= p.path_no_query.size()) {
            std::size_t slash = p.path_no_query.find('/', start);
            std::size_t end = (slash == std::string::npos)
                                  ? p.path_no_query.size()
                                  : slash;
            std::string seg(p.path_no_query.data() + start, end - start);
            if (!seg.empty()) {
                d.segs.push_back(sanitize_segment(url_decode(seg)));
            }
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        if (p.path_no_query.empty() ||
            (!p.path_no_query.empty() && p.path_no_query.back() == '/')) {
            // Matches path_for()'s "index" fallback so collision
            // detection below compares apples to apples.
            d.segs.emplace_back("index");
        }
        decoded.push_back(std::move(d));
    }

    // All URLs must agree on the host for us to drop it; otherwise the
    // host directory is load-bearing and has to stay.
    const std::string& host0 = decoded.front().host;
    bool same_host = true;
    for (const auto& d : decoded) {
        if (d.host != host0) { same_host = false; break; }
    }
    if (!same_host) return;

    // Longest common prefix across all segment lists.  We cap it at
    // (min_segments - 1) so the final filename always survives -- two
    // URLs that differ only in their last segment must still produce
    // distinct paths.
    std::size_t min_segs = decoded.front().segs.size();
    for (const auto& d : decoded) {
        if (d.segs.size() < min_segs) min_segs = d.segs.size();
    }
    std::size_t common = 0;
    const std::size_t max_common = (min_segs == 0) ? 0 : (min_segs - 1);
    while (common < max_common) {
        const std::string& probe = decoded.front().segs[common];
        bool all_match = true;
        for (const auto& d : decoded) {
            if (d.segs[common] != probe) { all_match = false; break; }
        }
        if (!all_match) break;
        ++common;
    }

    strip_host_ = host0;
    strip_segments_ = common;

    // Sanity check: after trimming, every URL should still map to a
    // unique local path.  If two URLs happen to collide (possible when
    // the only distinguishing segment was inside the common prefix --
    // e.g. only differing in host, which we already ruled out, but
    // keep the guard for future-proofing), back off the trim until
    // collisions disappear.
    auto collides = [&](std::size_t strip) {
        std::vector<std::string> joined;
        joined.reserve(decoded.size());
        for (const auto& d : decoded) {
            std::string j;
            for (std::size_t i = strip; i < d.segs.size(); ++i) {
                if (!j.empty()) j.push_back('/');
                j.append(d.segs[i]);
            }
            joined.push_back(std::move(j));
        }
        std::sort(joined.begin(), joined.end());
        for (std::size_t i = 1; i < joined.size(); ++i) {
            if (joined[i] == joined[i - 1]) return true;
        }
        return false;
    };
    while (strip_segments_ > 0 && collides(strip_segments_)) {
        --strip_segments_;
    }
}

bool UrlCache::run_curl(const std::string& url,
                         const std::filesystem::path& dest) {
    // Download to a sibling .part file first, then rename into place so
    // a partial download never masquerades as a complete cache hit.
    std::filesystem::path tmp = dest;
    tmp += ".part";

    // curl flags:
    //   -f : fail on HTTP errors (4xx/5xx) so we don't cache error pages
    //   -L : follow redirects (pre-signed S3/COS URLs redirect a lot)
    //   -sS: silent progress but still print errors to stderr
    //   --retry 2 / --retry-delay 1 : survive transient network flaps
    //   --connect-timeout 15 / --max-time 120 : bound worst-case hangs
    //   -o <path> : stream body to file (constant memory)
    std::ostringstream cmd;
    cmd << "curl -fLsS --retry 2 --retry-delay 1"
        << " --connect-timeout 15 --max-time 120"
        << " -o " << shell_quote(tmp.string())
        << " "    << shell_quote(url)
        << " 2>&1";

    std::string captured;
    captured.reserve(256);
#ifdef _WIN32
    FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
    FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
    if (!pipe) {
        last_error_ = "failed to launch curl (popen): " +
                      std::string(std::strerror(errno));
        return false;
    }
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        captured.append(buf);
        if (captured.size() > 4096) {
            captured.resize(4096);
            break;
        }
    }
#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif

    std::error_code ec;
    if (rc != 0) {
        std::filesystem::remove(tmp, ec);  // best-effort cleanup
        while (!captured.empty() &&
               (captured.back() == '\n' || captured.back() == '\r' ||
                captured.back() == ' ')) {
            captured.pop_back();
        }
        last_error_ = captured.empty()
            ? ("curl exit code " + std::to_string(rc))
            : captured;
        return false;
    }

    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        // Fall back to copy+delete across filesystem boundaries.
        std::filesystem::copy_file(tmp, dest,
            std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        if (ec) {
            last_error_ = "failed to finalize cache entry: " + ec.message();
            return false;
        }
    }
    last_error_.clear();
    return true;
}

std::filesystem::path UrlCache::fetch(const std::string& url,
                                       bool force_refresh) {
    namespace fs = std::filesystem;
    last_error_.clear();

    fs::path target = path_for(url);
    std::error_code ec;

    // Fast path: file already cached.  Require non-zero size so a stale
    // zero-byte file from an earlier crash doesn't pose as a hit.
    if (!force_refresh) {
        auto sz = fs::file_size(target, ec);
        if (!ec && sz > 0) return target;
    }

    fs::create_directories(target.parent_path(), ec);
    if (ec && !fs::exists(target.parent_path())) {
        last_error_ = "cannot create cache directory: " + ec.message();
        return {};
    }

    if (!run_curl(url, target)) {
        return {};
    }
    return target;
}

} // namespace idiff
