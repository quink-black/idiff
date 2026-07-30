#ifndef IDIFF_DOMAIN_GROUP_KEY_H
#define IDIFF_DOMAIN_GROUP_KEY_H

// Group-key extraction for filename-based image grouping.
//
// The group key is the filename stem: everything before the last
// extension dot.  Files from different directories with the same
// stem are considered the same group.  No suffix stripping is
// applied.

#include <string>
#include <string_view>

namespace idiff {

// Image-list grouping mode.  Controls how entries are sorted,
// visually separated, and click-selected in the image-list panel.
//   None     -- flat list, no group separators
//   ByName   -- group by filename stem (files with the same stem
//               from different directories form one group)
//   ByFolder -- group by parent directory (files in the same
//               directory form one group)
enum class GroupMode {
    None,
    ByName,
    ByFolder,
};

// Split a filename into (stem, extension) at the last '.' that is
// not the leading character.  A name with no extension or a
// single-character name returns the whole string as the stem and an
// empty extension.
std::pair<std::string_view, std::string_view>
split_stem_ext(std::string_view name);

// Extract the group key from a filename.  Path separators ('/' and
// '\\') are stripped before splitting, so values like
// "subdir/name.ext" and "name.ext" produce the same key.  Returns
// everything after the last separator and before the last
// extension dot, or the whole string when there is no extension.
std::string group_key_from_filename(const std::string& filename);

// Extract the group key from a filesystem path's parent directory.
// Returns everything before the last path separator, normalized to
// '/' separators.  Returns an empty string when the path has no
// directory component.  Used by GroupMode::ByFolder so files in the
// same directory form one group.
std::string group_key_from_directory(const std::string& path);

} // namespace idiff

#endif // IDIFF_DOMAIN_GROUP_KEY_H
