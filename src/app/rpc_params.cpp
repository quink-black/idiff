#include "app/rpc_params.h"

#include "app/app.h"
#include "app/rpc/rpc_dispatcher.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <system_error>

namespace idiff {
namespace rpc_params {

using nlohmann::json;
using rpc::ErrorCode;
using rpc::RpcException;

namespace {

[[noreturn]] void invalid(std::string msg) {
    throw RpcException(ErrorCode::InvalidParams, std::move(msg));
}

// The path with symlinks and "." / ".." resolved; nullopt for URLs and
// for paths the filesystem cannot resolve.
std::optional<std::filesystem::path> canonical_path(const std::string& p) {
    if (p.find("://") != std::string::npos) return std::nullopt;
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(std::filesystem::path(p), ec);
    if (ec) return std::nullopt;
    return c;
}

std::vector<int> canonical_matches(const std::string& path,
                                   const std::vector<ImageEntry>& entries) {
    std::vector<int> matches;
    const auto want = canonical_path(path);
    if (!want) return matches;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto have = canonical_path(entries[i].path);
        if (have && *have == *want) matches.push_back(static_cast<int>(i));
    }
    return matches;
}

} // namespace

const json& require_object(const json& params) {
    if (!params.is_object()) {
        invalid("params must be a JSON object");
    }
    return params;
}

int require_int_field(const json& params, const char* key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_number_integer()) {
        invalid(std::string("missing or non-integer field: ") + key);
    }
    return it->get<int>();
}

const std::string& require_string_field(const json& params, const char* key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_string()) {
        invalid(std::string("missing or non-string field: ") + key);
    }
    return it->get_ref<const std::string&>();
}

void check_index(int idx, std::size_t size, const char* what) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= size) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "%s out of range: %d (have %zu entries)",
                      what, idx, size);
        invalid(buf);
    }
}

int resolve_entry(const json& ref, const std::vector<ImageEntry>& entries,
                  const char* what) {
    if (ref.is_number_integer()) {
        int idx = ref.get<int>();
        check_index(idx, entries.size(), what);
        return idx;
    }
    if (!ref.is_string()) {
        invalid(std::string(what) +
                " must be an entry index (integer) or path (string)");
    }

    const auto& path = ref.get_ref<const std::string&>();
    std::vector<int> matches;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].path == path) matches.push_back(static_cast<int>(i));
    }
    if (matches.empty()) matches = canonical_matches(path, entries);
    if (matches.empty()) {
        invalid(std::string(what) + ": no entry has path \"" + path + "\"");
    }
    if (matches.size() > 1) {
        std::string list;
        for (int m : matches) {
            if (!list.empty()) list += ", ";
            list += std::to_string(m);
        }
        invalid(std::string(what) + ": path \"" + path +
                "\" matches entries " + list + "; pass an index instead");
    }
    return matches.front();
}

std::vector<int> resolve_entries(const json& refs,
                                 const std::vector<ImageEntry>& entries,
                                 const char* what) {
    if (!refs.is_array()) {
        invalid(std::string(what) + " must be an array");
    }
    std::vector<int> out;
    out.reserve(refs.size());
    const std::string elem = std::string(what) + "[]";
    for (const auto& r : refs) {
        out.push_back(resolve_entry(r, entries, elem.c_str()));
    }
    return out;
}

int require_entry_field(const json& params,
                        const std::vector<ImageEntry>& entries) {
    auto idx = params.find("index");
    auto path = params.find("path");
    const bool has_idx = idx != params.end();
    const bool has_path = path != params.end();
    if (has_idx == has_path) {
        invalid("pass exactly one of: index, path");
    }
    if (has_idx) {
        if (!idx->is_number_integer()) invalid("index must be an integer");
        return resolve_entry(*idx, entries, "index");
    }
    if (!path->is_string()) invalid("path must be a string");
    return resolve_entry(*path, entries, "path");
}

} // namespace rpc_params
} // namespace idiff
