#include <catch2/catch_test_macros.hpp>

#include "domain/action_registry.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>

using idiff::ActionOrigin;
using idiff::ActionRegistry;
using idiff::KeyChord;
using idiff::parse_key_chord;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

class TempFile {
public:
    explicit TempFile(const std::string& content) {
        std::random_device rd;
        path_ = fs::temp_directory_path() /
                ("idiff_actions_" + std::to_string(rd()) + ".json");
        std::ofstream(path_, std::ios::binary) << content;
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

} // namespace

TEST_CASE("parse_key_chord: modifiers and keys", "[action_registry]") {
    KeyChord c;
    REQUIRE(parse_key_chord("alt+1", c));
    REQUIRE(c == KeyChord{false, true, false, "1"});

    REQUIRE(parse_key_chord("Ctrl+Shift+K", c));
    REQUIRE(c == KeyChord{true, false, true, "k"});

    REQUIRE(parse_key_chord("f12", c));
    REQUIRE(c == KeyChord{false, false, false, "f12"});
}

TEST_CASE("parse_key_chord: rejects unknown input", "[action_registry]") {
    KeyChord c;
    REQUIRE_FALSE(parse_key_chord("", c));
    REQUIRE_FALSE(parse_key_chord("alt+", c));
    REQUIRE_FALSE(parse_key_chord("meta+a", c));
    REQUIRE_FALSE(parse_key_chord("f13", c));
    REQUIRE_FALSE(parse_key_chord("f0", c));
    REQUIRE_FALSE(parse_key_chord("f99999999999999999", c));
    REQUIRE_FALSE(parse_key_chord("f1x", c));
    REQUIRE_FALSE(parse_key_chord("ab", c));
    REQUIRE_FALSE(parse_key_chord("alt+alt", c));
}

TEST_CASE("set_session: stores buttons with their shortcuts",
          "[action_registry]") {
    ActionRegistry reg;
    auto err = reg.set_session(json::parse(R"([
        {"label": "Good", "command": ["tool", "good"], "key": "alt+1"},
        {"label": "Bad", "command": ["tool", "bad"]}
    ])"));
    REQUIRE(err.empty());
    REQUIRE(reg.buttons().size() == 2);
    REQUIRE(reg.buttons()[0].label == "Good");
    REQUIRE(reg.buttons()[0].command == std::vector<std::string>{"tool", "good"});
    REQUIRE(reg.buttons()[0].origin == ActionOrigin::Session);
    REQUIRE(reg.buttons()[0].chord == KeyChord{false, true, false, "1"});
    REQUIRE(reg.buttons()[1].chord == std::nullopt);
}

TEST_CASE("set_session: an invalid list leaves the registry unchanged",
          "[action_registry]") {
    ActionRegistry reg;
    REQUIRE(reg.set_session(json::parse(
        R"([{"label": "A", "command": ["a"]}])")).empty());

    const char* bad[] = {
        R"({"label": "A"})",
        R"([1])",
        R"([{"command": ["a"]}])",
        R"([{"label": "", "command": ["a"]}])",
        R"([{"label": "A"}])",
        R"([{"label": "A", "command": []}])",
        R"([{"label": "A", "command": [1]}])",
        R"([{"label": "A", "command": [""]}])",
        R"([{"label": "A", "command": ["a"], "key": "bogus+1"}])",
        R"([{"label": "A", "command": ["a"], "key": 5}])",
        R"([{"label": "A", "command": ["a"], "key": "f99999999999999999"}])",
    };
    for (const char* text : bad) {
        INFO(text);
        REQUIRE_FALSE(reg.set_session(json::parse(text)).empty());
        REQUIRE(reg.buttons().size() == 1);
        REQUIRE(reg.buttons()[0].label == "A");
    }
}

TEST_CASE("set_session: an empty list clears the session buttons",
          "[action_registry]") {
    ActionRegistry reg;
    REQUIRE(reg.set_session(json::parse(
        R"([{"label": "A", "command": ["a"]}])")).empty());
    REQUIRE(reg.set_session(json::array()).empty());
    REQUIRE(reg.buttons().empty());
}

TEST_CASE("set_session: the error names the offending element",
          "[action_registry]") {
    ActionRegistry reg;
    auto err = reg.set_session(json::parse(R"([
        {"label": "A", "command": ["a"]},
        {"label": "B"}
    ])"));
    REQUIRE(err.find("actions[1]") != std::string::npos);
}

TEST_CASE("load_user_file: accepts an array or an object",
          "[action_registry]") {
    {
        TempFile f(R"([{"label": "A", "command": ["a"]}])");
        ActionRegistry reg;
        REQUIRE(reg.load_user_file(f.path()).empty());
        REQUIRE(reg.buttons().size() == 1);
        REQUIRE(reg.buttons()[0].origin == ActionOrigin::User);
    }
    {
        TempFile f(R"({"actions": [{"label": "B", "command": ["b"]}]})");
        ActionRegistry reg;
        REQUIRE(reg.load_user_file(f.path()).empty());
        REQUIRE(reg.buttons()[0].label == "B");
    }
}

TEST_CASE("load_user_file: a missing file is not an error",
          "[action_registry]") {
    ActionRegistry reg;
    REQUIRE(reg.load_user_file("/nonexistent/idiff/actions.json").empty());
    REQUIRE(reg.buttons().empty());
}

TEST_CASE("load_user_file: malformed files are reported", "[action_registry]") {
    ActionRegistry reg;
    {
        TempFile f("not json");
        REQUIRE_FALSE(reg.load_user_file(f.path()).empty());
    }
    {
        TempFile f(R"({"other": []})");
        REQUIRE_FALSE(reg.load_user_file(f.path()).empty());
    }
    {
        TempFile f(R"([{"label": "A"}])");
        REQUIRE_FALSE(reg.load_user_file(f.path()).empty());
    }
    REQUIRE(reg.buttons().empty());
}

TEST_CASE("buttons: user buttons precede session buttons",
          "[action_registry]") {
    TempFile f(R"([{"label": "User", "command": ["u"], "key": "alt+1"}])");
    ActionRegistry reg;
    REQUIRE(reg.set_session(json::parse(
        R"([{"label": "Session", "command": ["s"], "key": "alt+1"}])")).empty());
    REQUIRE(reg.load_user_file(f.path()).empty());

    REQUIRE(reg.buttons().size() == 2);
    REQUIRE(reg.buttons()[0].label == "User");
    REQUIRE(reg.buttons()[1].label == "Session");
}
