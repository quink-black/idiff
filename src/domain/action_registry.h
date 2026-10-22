#ifndef IDIFF_DOMAIN_ACTION_REGISTRY_H
#define IDIFF_DOMAIN_ACTION_REGISTRY_H

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace idiff {

// A keyboard shortcut.  `key` is lower case: "a".."z", "0".."9" or
// "f1".."f12".
struct KeyChord {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    std::string key;

    bool operator==(const KeyChord& o) const noexcept {
        return ctrl == o.ctrl && alt == o.alt && shift == o.shift &&
               key == o.key;
    }
};

// Parses text such as "alt+1" or "ctrl+shift+k".  Returns false when a
// modifier or the key is unknown or the key is missing.
bool parse_key_chord(const std::string& text, KeyChord& out);

enum class ActionOrigin { User, Session };

// A button that runs an external command.
struct ActionButton {
    std::string label;
    std::vector<std::string> command;  // argv; no shell is involved
    std::string key_text;              // as written; empty without shortcut
    std::optional<KeyChord> chord;
    ActionOrigin origin = ActionOrigin::User;
};

// The buttons shown in the actions panel.  Two sources feed it: the
// user's actions file, read at startup, and the session set, which a
// client replaces through RPC and which lives as long as the process.
// Not thread safe.
class ActionRegistry {
public:
    // Replaces the session buttons with `actions`, a JSON array of
    // objects with "label", "command" and an optional "key".  Returns an
    // empty string on success, otherwise a message naming the offending
    // element; the registry is unchanged in that case.
    std::string set_session(const nlohmann::json& actions);

    // Replaces the user buttons with the contents of `path`: a JSON array
    // as above, or an object whose "actions" member is one.  A missing
    // file is not an error and yields no user buttons.  Returns an empty
    // string on success, otherwise a message; the registry is unchanged
    // in that case.
    std::string load_user_file(const std::filesystem::path& path);

    // User buttons first, then session buttons, each in source order.
    // When two buttons share a shortcut, the earlier one takes it.
    const std::vector<ActionButton>& buttons() const noexcept {
        return merged_;
    }

private:
    void rebuild();

    std::vector<ActionButton> user_;
    std::vector<ActionButton> session_;
    std::vector<ActionButton> merged_;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_ACTION_REGISTRY_H
