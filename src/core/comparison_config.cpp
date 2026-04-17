#include "core/comparison_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>

namespace idiff {
namespace {

using nlohmann::json;

// Case-insensitive lookup of the first key in `candidates` that exists
// on `obj`.  Returns nullptr if none match.  Case-insensitive so we
// tolerate snake_case vs. camelCase vs. PascalCase without a separate
// entry per spelling.
const json* find_first_key(const json& obj,
                            std::initializer_list<std::string_view> candidates) {
    if (!obj.is_object()) return nullptr;
    auto lower = [](std::string s) {
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
        }
        return s;
    };
    for (auto cand : candidates) {
        std::string needle = lower(std::string(cand));
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (lower(it.key()) == needle) {
                return &it.value();
            }
        }
    }
    return nullptr;
}

// Coerce any scalar-ish JSON node to a string.  Numbers and booleans
// get a sensible text representation; arrays / objects return empty so
// we don't accidentally stringify an entire sub-document into a label.
std::string to_string_flex(const json& v) {
    if (v.is_null()) return {};
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return {};
}

// Extract a (url, title, description) triple from a JSON node that is
// expected to describe one image.  Returns false when no URL could be
// found -- callers use that to silently skip malformed entries rather
// than aborting the whole load.
bool read_item(const json& node, ComparisonItem& out) {
    if (node.is_string()) {
        out.url = node.get<std::string>();
        return !out.url.empty();
    }
    if (!node.is_object()) return false;

    const json* url_node =
        find_first_key(node, {"url", "src", "href", "link", "path", "image"});
    if (!url_node) return false;
    out.url = to_string_flex(*url_node);
    if (out.url.empty()) return false;

    if (const json* t = find_first_key(node,
            {"title", "name", "label", "caption", "tag"})) {
        out.title = to_string_flex(*t);
    }
    if (const json* d = find_first_key(node,
            {"description", "desc", "detail", "note", "comment"})) {
        out.description = to_string_flex(*d);
    }
    return true;
}

// Read a single group.  `node` may be an object (with an explicit
// items list) or an array (treated as an items list directly).  Groups
// that end up with zero items are dropped by the caller so the UI does
// not display empty rows.
bool read_group(const json& node, ComparisonGroup& out) {
    if (node.is_array()) {
        for (const auto& item : node) {
            ComparisonItem ci;
            if (read_item(item, ci)) out.items.push_back(std::move(ci));
        }
        return !out.items.empty();
    }
    if (!node.is_object()) return false;

    if (const json* n = find_first_key(node, {"name", "title", "label", "id"})) {
        out.name = to_string_flex(*n);
    }
    if (const json* d = find_first_key(node,
            {"description", "desc", "detail", "note"})) {
        out.description = to_string_flex(*d);
    }

    const json* items = find_first_key(node,
        {"images", "items", "pictures", "files", "urls", "assets", "photos"});

    if (items && items->is_array()) {
        for (const auto& item : *items) {
            ComparisonItem ci;
            if (read_item(item, ci)) out.items.push_back(std::move(ci));
        }
    } else {
        // No dedicated items field.  As a last resort, scan the object
        // for any string value that looks like a URL -- this rescues
        // hand-written configs like { "orig": "http://...", "srA": "..." }.
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string& v = it.value().get_ref<const std::string&>();
            if (v.rfind("http://", 0) == 0 ||
                v.rfind("https://", 0) == 0 ||
                v.rfind("file://", 0) == 0) {
                ComparisonItem ci;
                ci.url = v;
                ci.title = it.key();
                out.items.push_back(std::move(ci));
            }
        }
    }
    return !out.items.empty();
}

// Decide how to interpret a top-level array: is it a list of groups,
// or a list of items?  If any element looks like a group (object with
// an items-ish key or an array), we treat the whole thing as groups.
// Otherwise we roll every element into one synthetic group.
void read_top_level_array(const json& arr, std::vector<ComparisonGroup>& out) {
    bool looks_like_groups = false;
    for (const auto& e : arr) {
        if (e.is_array()) { looks_like_groups = true; break; }
        if (e.is_object() && find_first_key(e, {
                "images", "items", "pictures", "files", "urls", "assets",
                "photos"})) {
            looks_like_groups = true;
            break;
        }
    }

    if (looks_like_groups) {
        for (const auto& e : arr) {
            ComparisonGroup g;
            if (read_group(e, g)) out.push_back(std::move(g));
        }
    } else {
        ComparisonGroup g;
        for (const auto& e : arr) {
            ComparisonItem ci;
            if (read_item(e, ci)) g.items.push_back(std::move(ci));
        }
        if (!g.items.empty()) out.push_back(std::move(g));
    }
}

} // namespace

ComparisonConfig load_comparison_config(const std::string& path) {
    ComparisonConfig cfg;
    cfg.source_path = path;

    std::ifstream in(path);
    if (!in.is_open()) {
        cfg.error = "cannot open config file: " + path;
        return cfg;
    }

    json doc;
    try {
        in >> doc;
    } catch (const json::parse_error& ex) {
        cfg.error = std::string("JSON parse error: ") + ex.what();
        return cfg;
    } catch (const std::exception& ex) {
        cfg.error = std::string("JSON read error: ") + ex.what();
        return cfg;
    }

    if (doc.is_array()) {
        read_top_level_array(doc, cfg.groups);
    } else if (doc.is_object()) {
        // Look for a groups-like field first.
        const json* groups = find_first_key(doc, {
            "comparisonGroups", "comparison_groups", "groups", "cases",
            "sets", "collections"});
        if (groups && groups->is_array()) {
            for (const auto& g : *groups) {
                ComparisonGroup cg;
                if (read_group(g, cg)) cfg.groups.push_back(std::move(cg));
            }
        } else {
            // No explicit groups -- treat the top-level object as one
            // flat group (common case for ad-hoc configs).
            ComparisonGroup cg;
            if (read_group(doc, cg)) cfg.groups.push_back(std::move(cg));
        }
    } else {
        cfg.error = "unsupported JSON top-level type (expected object or array)";
        return cfg;
    }

    // Auto-name any groups that didn't carry an explicit label so the
    // UI can present them unambiguously.
    int unnamed = 0;
    for (std::size_t i = 0; i < cfg.groups.size(); ++i) {
        if (cfg.groups[i].name.empty()) {
            cfg.groups[i].name = "Group " + std::to_string(++unnamed);
        }
    }

    if (cfg.groups.empty()) {
        cfg.error = "no image groups found in " + path;
    }
    return cfg;
}

} // namespace idiff
