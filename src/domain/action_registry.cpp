#include "domain/action_registry.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <sstream>

namespace idiff {

namespace {

constexpr std::size_t kMaxButtons = 64;

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_valid_key_name(const std::string& k) {
    if (k.size() == 1) {
        const char c = k[0];
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    }
    if (k.size() >= 2 && k.size() <= 3 && k[0] == 'f') {
        for (std::size_t i = 1; i < k.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(k[i]))) return false;
        }
        const int n = std::stoi(k.substr(1));
        return n >= 1 && n <= 12;
    }
    return false;
}

std::string parse_buttons(const nlohmann::json& arr, ActionOrigin origin,
                          std::vector<ActionButton>& out) {
    if (!arr.is_array()) return "actions must be an array";
    if (arr.size() > kMaxButtons) {
        return "too many actions (at most " + std::to_string(kMaxButtons) + ")";
    }
    std::vector<ActionButton> parsed;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& item = arr[i];
        const std::string where = "actions[" + std::to_string(i) + "]: ";
        if (!item.is_object()) return where + "must be an object";

        ActionButton b;
        b.origin = origin;

        auto label = item.find("label");
        if (label == item.end() || !label->is_string() ||
            label->get<std::string>().empty()) {
            return where + "label must be a non-empty string";
        }
        b.label = label->get<std::string>();

        auto cmd = item.find("command");
        if (cmd == item.end() || !cmd->is_array() || cmd->empty()) {
            return where + "command must be a non-empty array of strings";
        }
        for (const auto& arg : *cmd) {
            if (!arg.is_string()) {
                return where + "command must be a non-empty array of strings";
            }
            b.command.push_back(arg.get<std::string>());
        }
        if (b.command[0].empty()) {
            return where + "command[0] must not be empty";
        }

        auto key = item.find("key");
        if (key != item.end() && !key->is_null()) {
            if (!key->is_string()) return where + "key must be a string";
            b.key_text = key->get<std::string>();
            if (!b.key_text.empty()) {
                KeyChord chord;
                if (!parse_key_chord(b.key_text, chord)) {
                    return where + "unsupported key \"" + b.key_text + "\"";
                }
                b.chord = chord;
            }
        }
        parsed.push_back(std::move(b));
    }
    out = std::move(parsed);
    return {};
}

} // namespace

bool parse_key_chord(const std::string& text, KeyChord& out) {
    KeyChord chord;
    std::string rest = to_lower(text);
    std::size_t pos = 0;
    for (;;) {
        const std::size_t plus = rest.find('+', pos);
        const std::string part = rest.substr(
            pos, plus == std::string::npos ? std::string::npos : plus - pos);
        if (plus == std::string::npos) {
            if (!is_valid_key_name(part)) return false;
            chord.key = part;
            break;
        }
        if (part == "ctrl") chord.ctrl = true;
        else if (part == "alt") chord.alt = true;
        else if (part == "shift") chord.shift = true;
        else return false;
        pos = plus + 1;
    }
    out = chord;
    return true;
}

std::string ActionRegistry::set_session(const nlohmann::json& actions) {
    std::vector<ActionButton> parsed;
    std::string err = parse_buttons(actions, ActionOrigin::Session, parsed);
    if (!err.empty()) return err;
    session_ = std::move(parsed);
    rebuild();
    return {};
}

std::string ActionRegistry::load_user_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        user_.clear();
        rebuild();
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return path.string() + ": cannot open";
    std::stringstream buf;
    buf << in.rdbuf();

    nlohmann::json doc = nlohmann::json::parse(buf.str(), nullptr, false);
    if (doc.is_discarded()) return path.string() + ": invalid JSON";

    const nlohmann::json* arr = &doc;
    if (doc.is_object()) {
        auto it = doc.find("actions");
        if (it == doc.end()) return path.string() + ": missing \"actions\"";
        arr = &*it;
    }
    std::vector<ActionButton> parsed;
    std::string err = parse_buttons(*arr, ActionOrigin::User, parsed);
    if (!err.empty()) return path.string() + ": " + err;
    user_ = std::move(parsed);
    rebuild();
    return {};
}

void ActionRegistry::rebuild() {
    merged_ = user_;
    merged_.insert(merged_.end(), session_.begin(), session_.end());
}

} // namespace idiff
