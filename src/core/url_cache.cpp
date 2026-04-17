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
#include <optional>
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

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4, single-shot).
//
// We need a deterministic, short fingerprint of the comparison-config
// JSON for the cache directory name.  SHA-256 is overkill for a
// dedup key (we only keep 12 hex chars = 48 bits), but rolling our
// own keeps url_cache.cpp free of crypto-library dependencies and
// portable across every platform idiff already supports.  The
// implementation below is a direct, unoptimized transcription of
// FIPS 180-4 §6.2 -- sufficient for hashing JSON files up to a few
// MB with negligible cost.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline std::uint32_t rotr32(std::uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

std::string sha256_hex(const std::string& data) {
    std::uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    // Pad: data || 0x80 || 0x00... || 64-bit big-endian bit length.
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8ull;
    std::string buf = data;
    buf.push_back('\x80');
    while ((buf.size() % 64) != 56) buf.push_back('\x00');
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));
    }

    for (std::size_t off = 0; off < buf.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(buf.data() + off + i * 4);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) |
                   (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) <<  8) |
                   (static_cast<std::uint32_t>(p[3]));
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
            std::uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2],  19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
            std::uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int s = 28; s >= 0; s -= 4) {
            out.push_back(hex[(h[i] >> s) & 0xf]);
        }
    }
    return out;
}

// Slurp an entire small file into memory.  Returns std::nullopt on any
// I/O failure so the caller can branch cleanly into its fallback path
// instead of having to inspect errno.  Intended for JSON configs
// (typically << 1 MB); there is no size cap because a pathological
// config would already have failed to parse upstream.
std::optional<std::string> read_file_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream oss;
    oss << in.rdbuf();
    if (!in && !in.eof()) return std::nullopt;
    return oss.str();
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

std::filesystem::path UrlCache::prepare_for_config(
    const std::filesystem::path& cache_root,
    const std::filesystem::path& json_file,
    std::string* out_status) {
    namespace fs = std::filesystem;

    auto set_status = [&](std::string s) {
        if (out_status) *out_status = std::move(s);
    };

    // Derive the directory stem from the JSON file name.  Sanitizing
    // keeps the filesystem happy when the config lives under a path
    // with spaces, unicode, colons, etc.
    std::string stem = json_file.stem().string();
    stem = sanitize_segment(stem);
    if (stem.empty()) stem = "config";

    // Read the JSON bytes up front: we need them for both the hash
    // (directory name) and the on-disk "source.json" marker used to
    // confirm the reused directory actually corresponds to this
    // config's content.  Anything that fails here forces us into a
    // timestamped fallback so a mis-readable file never silently
    // pollutes an unrelated cache.
    auto bytes = read_file_bytes(json_file);
    if (!bytes) {
        // Fall back to a timestamped, content-less directory so the
        // caller still gets a usable cache even if we cannot stat
        // the JSON right now (e.g. permission glitch, disappearing
        // network mount).  This matches pre-hash behaviour.
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
        fs::path dir = cache_root / ("idiff_cache_" + stem + "_" + buf);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            set_status("Failed to create cache directory: " + ec.message());
            return {};
        }
        set_status("Could not read JSON; using fresh cache: " + dir.string());
        return dir;
    }

    // 12 hex chars = 48 bits.  At that width collisions among user
    // JSON files are vanishingly rare -- and the source.json check
    // below is the authoritative guard even when they do happen.
    std::string short_hash = sha256_hex(*bytes).substr(0, 12);
    fs::path primary = cache_root / ("idiff_cache_" + stem + "_" + short_hash);
    fs::path marker  = primary / "source.json";

    std::error_code ec;
    auto write_marker = [&](const fs::path& dir) -> bool {
        std::ofstream out(dir / "source.json", std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        return static_cast<bool>(out);
    };

    // Case 1: directory exists.  Trust it only when source.json is
    // byte-identical to the incoming JSON -- otherwise we are looking
    // at a hash collision (or a cache someone hand-edited) and must
    // not mix unrelated images in.
    if (fs::exists(primary, ec)) {
        auto existing = read_file_bytes(marker);
        if (existing && *existing == *bytes) {
            set_status("Reused cache: " + primary.string());
            return primary;
        }
        if (!existing) {
            // Directory exists but marker is missing.  This happens
            // when an older build created the directory without
            // recording the JSON -- adopt it by writing the marker
            // now, since the directory name already embeds the
            // content hash.
            if (write_marker(primary)) {
                set_status("Adopted cache: " + primary.string());
                return primary;
            }
            set_status("Failed to write source.json under " + primary.string());
            return {};
        }
        // Contents diverge: fall through to the collision branch.
    }

    // Case 2: collision fallback.  Append a timestamp so the existing
    // (mismatched) directory stays intact and the new session gets a
    // clean slate.  The timestamp ensures uniqueness even if the user
    // somehow triggers the same collision twice within one second.
    if (fs::exists(primary, ec)) {
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
        fs::path alt = cache_root /
            ("idiff_cache_" + stem + "_" + short_hash + "_" + buf);
        fs::create_directories(alt, ec);
        if (ec) {
            set_status("Failed to create fallback cache directory: " + ec.message());
            return {};
        }
        if (!write_marker(alt)) {
            set_status("Failed to write source.json under " + alt.string());
            return {};
        }
        set_status("Hash collision; using fallback cache: " + alt.string());
        return alt;
    }

    // Case 3: brand new primary directory.
    fs::create_directories(primary, ec);
    if (ec) {
        set_status("Failed to create cache directory: " + ec.message());
        return {};
    }
    if (!write_marker(primary)) {
        set_status("Failed to write source.json under " + primary.string());
        return {};
    }
    set_status("Created cache: " + primary.string());
    return primary;
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
    // per-config (resolve_default_root() / prepare_for_config()),
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
                         const std::filesystem::path& dest,
                         std::string* out_error) {
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

    // Report the error either to `*out_error` (thread-local scratch
    // for the background pool) or to the cache's own last_error_
    // string.  Keeping both paths separate means fetch() never
    // stomps on what a concurrent worker wrote, and vice versa.
    auto set_err = [&](std::string msg) {
        if (out_error) { *out_error = std::move(msg); }
        else           { last_error_ = std::move(msg); }
    };

    std::string captured;
    captured.reserve(256);
#ifdef _WIN32
    FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
    FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
    if (!pipe) {
        set_err("failed to launch curl (popen): " +
                std::string(std::strerror(errno)));
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
        set_err(captured.empty()
            ? ("curl exit code " + std::to_string(rc))
            : captured);
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
            set_err("failed to finalize cache entry: " + ec.message());
            return false;
        }
    }
    if (!out_error) last_error_.clear();
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

    // If a prefetch for this URL is already running, join it instead of
    // starting a duplicate curl.  This is the whole point of the
    // background pool from the foreground's perspective -- a group the
    // user selects while its images are still being prefetched should
    // not re-download anything, it should just wait.
    //
    // When force_refresh is requested we intentionally bypass the join:
    // the caller explicitly wants a fresh copy, so letting them reuse
    // an ongoing download (which might itself have started before the
    // reason for the refresh existed) would be incorrect.
    std::shared_ptr<Task> wait_on;
    if (!force_refresh) {
        std::unique_lock<std::mutex> lk(pool_mtx_);
        auto it = inflight_.find(url);
        if (it != inflight_.end()) {
            wait_on = it->second;
        }
    }
    if (wait_on) {
        std::unique_lock<std::mutex> tlk(wait_on->m);
        wait_on->cv.wait(tlk, [&] { return wait_on->done; });
        if (wait_on->success) {
            return wait_on->path;
        }
        // The prefetch failed; fall through and retry on this thread
        // so the caller still gets a shot at surfacing a fresh error
        // via last_error().  The inflight_ entry has already been
        // removed by the worker that produced the failure.
        last_error_ = wait_on->error;
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

bool UrlCache::is_cached(const std::string& url) const {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path_for(url), ec);
    return !ec && sz > 0;
}

void UrlCache::prefetch(const std::string& url, int priority) {
    if (url.empty()) return;

    // Skip anything already materialized on disk -- a prefetch request
    // for a hit is just noise.  Intentionally done outside the pool
    // lock: the file-existence check is pure I/O and the worst case
    // (file appears between our check and enqueue) is a redundant
    // curl that immediately returns because its .part rename will
    // overwrite a fresh but identical file.
    if (is_cached(url)) return;

    {
        std::lock_guard<std::mutex> lk(pool_mtx_);
        if (stop_) return;
        // Dedup against anything already queued or running.
        if (inflight_.count(url)) return;

        auto t = std::make_shared<Task>();
        t->url = url;
        t->priority = priority;
        t->seq = next_seq_++;
        inflight_.emplace(url, t);

        PendingRef ref;
        ref.priority = priority;
        ref.seq      = t->seq;
        ref.url      = url;
        queue_.push(std::move(ref));
    }
    pool_cv_.notify_one();

    // Start workers only once we actually have something to do.  The
    // check inside ensure_pool_started() is cheap on the hot path.
    ensure_pool_started();
}

void UrlCache::cancel_pending_prefetches() {
    std::lock_guard<std::mutex> lk(pool_mtx_);
    // We discard the entire priority_queue -- walking it to keep
    // "already started" entries isn't necessary because started tasks
    // aren't in the queue anymore (workers pop them off before
    // marking them started).  For each discarded URL we also drop
    // its inflight_ entry so a subsequent prefetch() call for the
    // same URL can re-queue it at the new priority.
    while (!queue_.empty()) {
        const auto& ref = queue_.top();
        auto it = inflight_.find(ref.url);
        if (it != inflight_.end() && it->second && !it->second->started) {
            // Wake up anybody waiting on this task with a "cancelled"
            // result so they can fall back to a synchronous fetch.
            auto task = it->second;
            {
                std::lock_guard<std::mutex> tlk(task->m);
                task->done    = true;
                task->success = false;
                task->error   = "cancelled";
            }
            task->cv.notify_all();
            inflight_.erase(it);
        }
        queue_.pop();
    }
}

void UrlCache::ensure_pool_started() {
    std::lock_guard<std::mutex> lk(pool_mtx_);
    if (pool_started_ || stop_) return;

    // Three workers is the sweet spot for mass HTTP downloads: enough
    // concurrency to mask per-request RTT, few enough to stay polite
    // to the origin and avoid triggering rate limits.  Tuning this up
    // rarely helps because curl is already pipelining retries.
    constexpr unsigned kWorkerCount = 3;
    workers_.reserve(kWorkerCount);
    for (unsigned i = 0; i < kWorkerCount; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
    pool_started_ = true;
}

void UrlCache::worker_loop() {
    namespace fs = std::filesystem;
    for (;;) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lk(pool_mtx_);
            pool_cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;

            // Pop the highest-priority pending URL.  A cancel_pending
            // call may have already erased its inflight_ entry, in
            // which case we just loop back and try the next one.
            auto ref = queue_.top();
            queue_.pop();
            auto it = inflight_.find(ref.url);
            if (it == inflight_.end()) continue;
            task = it->second;
            if (!task) { inflight_.erase(it); continue; }

            // Mark the task as started so cancel_pending_prefetches()
            // leaves it alone (we're committed to finishing the curl).
            {
                std::lock_guard<std::mutex> tlk(task->m);
                task->started = true;
            }
        }

        // Fast path: the file may have arrived on disk between enqueue
        // and now (e.g. a foreground fetch ran first).  Skip curl and
        // just publish a success result.
        fs::path target = path_for(task->url);
        std::error_code ec;
        auto sz = fs::file_size(target, ec);
        bool ok = !ec && sz > 0;
        std::string err;

        if (!ok) {
            fs::create_directories(target.parent_path(), ec);
            if (ec && !fs::exists(target.parent_path())) {
                err = "cannot create cache directory: " + ec.message();
            } else {
                ok = run_curl(task->url, target, &err);
            }
        }

        // Publish the result and wake anyone waiting on fetch().
        {
            std::lock_guard<std::mutex> tlk(task->m);
            task->path    = ok ? target : fs::path{};
            task->error   = ok ? std::string{} : std::move(err);
            task->success = ok;
            task->done    = true;
        }
        task->cv.notify_all();

        // Done; drop the inflight entry so future prefetch() calls
        // can re-queue this URL (e.g. after force_refresh deletion).
        {
            std::lock_guard<std::mutex> lk(pool_mtx_);
            auto it = inflight_.find(task->url);
            if (it != inflight_.end() && it->second == task) {
                inflight_.erase(it);
            }
        }
    }
}

UrlCache::~UrlCache() {
    {
        std::lock_guard<std::mutex> lk(pool_mtx_);
        stop_ = true;
        // Drain the queue so workers exit cleanly once their current
        // download finishes.  We do NOT touch Tasks already picked up
        // by a worker -- interrupting a curl mid-flight would leak a
        // zombie .part file.  worker_loop() notices stop_ after its
        // current iteration and returns.
        while (!queue_.empty()) {
            const auto& ref = queue_.top();
            auto it = inflight_.find(ref.url);
            if (it != inflight_.end() && it->second && !it->second->started) {
                auto t = it->second;
                {
                    std::lock_guard<std::mutex> tlk(t->m);
                    t->done    = true;
                    t->success = false;
                    t->error   = "cancelled";
                }
                t->cv.notify_all();
                inflight_.erase(it);
            }
            queue_.pop();
        }
    }
    pool_cv_.notify_all();
    for (auto& th : workers_) {
        if (th.joinable()) th.join();
    }
}

} // namespace idiff
