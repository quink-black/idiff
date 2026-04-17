#ifndef IDIFF_COMPARISON_CONFIG_H
#define IDIFF_COMPARISON_CONFIG_H

#include <string>
#include <vector>

namespace idiff {

// A single entry inside a group.  Only `url` is guaranteed to be
// populated; `title` and `description` are best-effort metadata that
// downstream UI can use for labelling, tooltips, etc.
struct ComparisonItem {
    std::string url;
    std::string title;
    std::string description;
};

// A named collection of items that are meant to be compared together.
// The parser recognizes groups under many different JSON shapes (see
// comparison_config.cpp for the supported keys).  `name` may be empty
// if the source document did not provide one -- callers should fall
// back to e.g. "Group N" in that case.
struct ComparisonGroup {
    std::string name;
    std::string description;
    std::vector<ComparisonItem> items;
};

// Result of parsing a comparison-config JSON file.  On success,
// `groups` is non-empty and `error` is empty.  On failure, `groups` is
// empty and `error` carries a human-readable reason.
struct ComparisonConfig {
    std::vector<ComparisonGroup> groups;
    std::string error;          // empty on success
    std::string source_path;    // path of the file we parsed (informational)
};

// Read and parse a comparison-config JSON file from disk.  The parser
// is intentionally lenient:
//
//   * Accepts a top-level object with any of "comparisonGroups",
//     "groups", "comparison_groups", "cases" as the group list.
//   * Accepts a top-level array where each element is either a group
//     object or a plain image-item object.  In the latter case, all
//     items become members of a single implicit group.
//   * Within a group, images are looked up under "images", "items",
//     "pictures", "files", "urls"; if the group object itself is an
//     array, its elements are treated as image items directly.
//   * Within an image item, the URL is looked up under "url", "src",
//     "href", "link", "path"; title under "title", "name", "label",
//     "caption"; description under "description", "desc", "detail".
//     A plain string is treated as just a URL.
//
// If the document cannot be parsed as JSON at all, or no URLs can be
// extracted, an error is returned with an empty group list.
ComparisonConfig load_comparison_config(const std::string& path);

} // namespace idiff

#endif // IDIFF_COMPARISON_CONFIG_H
