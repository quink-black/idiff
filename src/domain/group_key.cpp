#include "domain/group_key.h"

namespace idiff {

std::pair<std::string_view, std::string_view>
split_stem_ext(std::string_view name) {
    if (name.size() <= 1) return {name, {}};
    auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return {name, {}};
    return {name.substr(0, dot), name.substr(dot + 1)};
}

std::string group_key_from_filename(const std::string& filename) {
    auto [stem, ext] = split_stem_ext(filename);
    // ext is non-empty only when a dot was found past position 0.
    // When no extension exists the stem is the whole filename, which
    // is the correct group key.
    (void)ext;
    return std::string(stem);
}

} // namespace idiff
