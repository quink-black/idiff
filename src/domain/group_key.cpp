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
    // Strip any directory portion first.  The image-list panel may
    // feed us labels like "seedvr2_1x/role1.png" (set by the
    // comparison-config flow which overrides ImageEntry::filename
    // with a human-friendly path-style display_label).  Without
    // this step every such label has a unique stem and the
    // group-by-name view degenerates into "every entry is its own
    // group" -- clicks land on a singleton and the panel ends up
    // with selected entries scattered across multiple visual
    // groups.  Treat both '/' and '\\' as separators so the rule
    // also works on Windows-flavoured paths.
    std::string_view view(filename);
    auto sep = view.find_last_of("/\\");
    if (sep != std::string_view::npos) view.remove_prefix(sep + 1);

    auto [stem, ext] = split_stem_ext(view);
    // ext is non-empty only when a dot was found past position 0.
    // When no extension exists the stem is the whole filename, which
    // is the correct group key.
    (void)ext;
    return std::string(stem);
}

std::string group_key_from_directory(const std::string& path) {
    std::string_view view(path);
    auto sep = view.find_last_of("/\\");
    if (sep == std::string_view::npos) return {};
    std::string dir(view.substr(0, sep));
    for (auto& c : dir) {
        if (c == '\\') c = '/';
    }
    return dir;
}

} // namespace idiff
