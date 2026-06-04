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

// Split a filename into (stem, extension) at the last '.' that is
// not the leading character.  A name with no extension or a
// single-character name returns the whole string as the stem and an
// empty extension.
std::pair<std::string_view, std::string_view>
split_stem_ext(std::string_view name);

// Extract the group key from a filename.  Returns everything before
// the last extension dot, or the whole string when there is no
// extension.
std::string group_key_from_filename(const std::string& filename);

} // namespace idiff

#endif // IDIFF_DOMAIN_GROUP_KEY_H
