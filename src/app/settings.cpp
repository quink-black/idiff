#include "app/settings.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace idiff {

namespace {

// Trim ASCII whitespace from both ends.
std::string trim(std::string s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back()))  s.pop_back();
    return s;
}

// Parse one "key=value" line.  Returns false on malformed input; lines
// starting with '#' or blank are silently skipped by returning true with
// empty key.
bool parse_kv_line(const std::string& line, std::string& key, std::string& val) {
    key.clear();
    val.clear();
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') return true;
    auto eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = trim(t.substr(0, eq));
    val = trim(t.substr(eq + 1));
    return !key.empty();
}

// Accept a handful of common spellings so hand-edited files still load.
bool parse_bool(const std::string& s, bool fallback) {
    if (s == "1" || s == "true"  || s == "True"  || s == "TRUE"
                 || s == "yes"   || s == "on")   return true;
    if (s == "0" || s == "false" || s == "False" || s == "FALSE"
                 || s == "no"    || s == "off")  return false;
    return fallback;
}

// Pick a platform-appropriate directory for persistent app settings.
std::filesystem::path settings_dir() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) {
        return std::filesystem::path(appdata) / "idiff";
    }
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return std::filesystem::path(xdg) / "idiff";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
#ifdef __APPLE__
        return std::filesystem::path(home) / "Library" / "Application Support" / "idiff";
#else
        return std::filesystem::path(home) / ".config" / "idiff";
#endif
    }
#endif
    // Last resort: current working directory.
    return std::filesystem::path(".");
}

// Closed-set validators for the YUV colour metadata fields.  These
// must stay in sync with the Combo entries the dialog exposes; any
// value we won't ever round-trip through the UI is dropped on load
// so a stale settings file from an earlier build (which used
// human-readable names like "BT.2020 NCL" or "Full") cannot poison
// the dialog's initial state.  Pixel format is intentionally not
// validated here because the dialog allows a "Custom..." escape
// hatch for FFmpeg names not enumerated in the preset list.
bool is_valid_color_range(const std::string& v) {
    return v == "tv" || v == "pc";
}

bool is_valid_color_matrix(const std::string& v) {
    return v == "smpte170m" || v == "bt709" || v == "bt2020nc";
}

bool is_valid_color_primaries(const std::string& v) {
    return v == "smpte170m" || v == "bt709" || v == "bt2020";
}

bool is_valid_transfer(const std::string& v) {
    return v == "bt709" || v == "iec61966-2-1"
        || v == "smpte2084" || v == "arib-std-b67";
}

} // namespace

std::string AppSettings::default_path() {
    auto dir = settings_dir();
    if (dir == std::filesystem::path(".")) {
        return "idiff_settings.txt";
    }
    return (dir / "settings.txt").string();
}

AppSettings AppSettings::load(const std::string& path) {
    AppSettings s;
    std::string p = path.empty() ? default_path() : path;

    std::ifstream in(p);
    if (!in.is_open()) {
        return s;  // missing file is fine -- use defaults
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string key, val;
        if (!parse_kv_line(line, key, val) || key.empty()) continue;

        if (key == "yuv.width") {
            s.last_yuv_params.width = std::atoi(val.c_str());
        } else if (key == "yuv.height") {
            s.last_yuv_params.height = std::atoi(val.c_str());
        } else if (key == "yuv.pixel_format") {
            s.last_yuv_params.pixel_format = val;
        } else if (key == "yuv.color_range") {
            if (is_valid_color_range(val)) {
                s.last_yuv_params.color_range = val;
            }
        } else if (key == "yuv.color_matrix") {
            if (is_valid_color_matrix(val)) {
                s.last_yuv_params.color_matrix = val;
            }
        } else if (key == "yuv.color_primaries") {
            if (is_valid_color_primaries(val)) {
                s.last_yuv_params.color_primaries = val;
            }
        } else if (key == "yuv.transfer") {
            if (is_valid_transfer(val)) {
                s.last_yuv_params.transfer = val;
            }
        } else if (key == "viewport.show_ruler") {
            s.show_ruler = parse_bool(val, s.show_ruler);
        } else if (key == "viewport.show_grid") {
            s.show_grid = parse_bool(val, s.show_grid);
        } else if (key == "viewport.grid_layout") {
            s.grid_layout = std::atoi(val.c_str());
        } else if (key == "viewport.grid_cols") {
            s.grid_cols = std::atoi(val.c_str());
        } else if (key == "diff.heatmap_color") {
            s.heatmap_color = std::atoi(val.c_str());
        } else if (key == "diff.amplification") {
            s.diff_amplification = std::atof(val.c_str());
        } else if (key == "sr.scale") {
            s.sr_scale = std::atoi(val.c_str());
        } else if (key == "sr.tile_size") {
            s.sr_tile_size = std::atoi(val.c_str());
        } else if (key == "sr.tile_overlap") {
            s.sr_tile_overlap = std::atoi(val.c_str());
        } else if (key == "sr.model") {
            s.sr_model = val;
        } else if (key == "sr.color_correction") {
            s.sr_color_correction = val;
        } else if (key == "inspector.panel") {
            s.inspector_panel = std::atoi(val.c_str());
        } else if (key == "panel.show_image_list") {
            s.show_image_list = parse_bool(val, s.show_image_list);
        } else if (key == "panel.show_inspector") {
            s.show_inspector = parse_bool(val, s.show_inspector);
        } else if (key == "panel.group_by_name") {
            s.group_by_name = parse_bool(val, s.group_by_name);
        } else if (key == "viewport.upscale_method") {
            s.upscale_method = std::atoi(val.c_str());
        } else if (key == "viewport.channel_view_mode") {
            s.channel_view_mode = std::atoi(val.c_str());
        } else if (key == "viewport.view_background") {
            s.view_background = std::atoi(val.c_str());
        } else if (key == "viewport.comparison_mode") {
            s.comparison_mode = std::atoi(val.c_str());
        } else if (key == "viewport.loader_backend") {
            s.loader_backend = std::atoi(val.c_str());
        }
    }
    return s;
}

bool AppSettings::save(const std::string& path) const {
    std::string p = path.empty() ? default_path() : path;

    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(p).parent_path(), ec);
    if (ec && !std::filesystem::exists(
                  std::filesystem::path(p).parent_path())) {
        last_error = "cannot create settings directory: " + ec.message();
        return false;
    }

    std::ofstream out(p, std::ios::trunc);
    if (!out.is_open()) {
        last_error = std::string("cannot open settings file for writing: ")
                     + std::strerror(errno);
        return false;
    }

    out << "# idiff settings (auto-generated; safe to delete)\n";
    out << "yuv.width=" << last_yuv_params.width << "\n";
    out << "yuv.height=" << last_yuv_params.height << "\n";
    out << "yuv.pixel_format=" << last_yuv_params.pixel_format << "\n";
    out << "yuv.color_range=" << last_yuv_params.color_range << "\n";
    out << "yuv.color_matrix=" << last_yuv_params.color_matrix << "\n";
    out << "yuv.color_primaries=" << last_yuv_params.color_primaries << "\n";
    out << "yuv.transfer=" << last_yuv_params.transfer << "\n";
    out << "viewport.show_ruler=" << (show_ruler ? "true" : "false") << "\n";
    out << "viewport.show_grid="  << (show_grid  ? "true" : "false") << "\n";
    out << "viewport.grid_layout=" << grid_layout << "\n";
    out << "viewport.grid_cols="   << grid_cols   << "\n";
    out << "diff.heatmap_color="   << heatmap_color << "\n";
    out << "diff.amplification="   << diff_amplification << "\n";
    out << "sr.scale="             << sr_scale << "\n";
    out << "sr.tile_size="         << sr_tile_size << "\n";
    out << "sr.tile_overlap="      << sr_tile_overlap << "\n";
    out << "sr.model="             << sr_model << "\n";
    out << "sr.color_correction="  << sr_color_correction << "\n";
    out << "inspector.panel="      << inspector_panel << "\n";
    out << "panel.show_image_list=" << (show_image_list ? "true" : "false") << "\n";
    out << "panel.show_inspector="  << (show_inspector  ? "true" : "false") << "\n";
    out << "panel.group_by_name="   << (group_by_name   ? "true" : "false") << "\n";
    out << "viewport.upscale_method="    << upscale_method    << "\n";
    out << "viewport.channel_view_mode=" << channel_view_mode << "\n";
    out << "viewport.view_background="   << view_background   << "\n";
    out << "viewport.comparison_mode="   << comparison_mode   << "\n";
    out << "viewport.loader_backend="    << loader_backend    << "\n";
    if (!out) {
        last_error = "I/O error while writing settings";
        return false;
    }
    last_error.clear();
    return true;
}

} // namespace idiff
