// Integration tests for the group-aware selection RPC surface:
//
//   * selection.select_group selects every entry sharing a filename
//     stem with the given index, and reports {changed, indices}.
//   * selection.select_range selects the inclusive [from, to] range
//     and reports {changed, indices}.
//   * Both reject out-of-range and malformed params with InvalidParams.
//
// What this covers and what it does not
// -------------------------------------
// As with test_rpc_group_by_name.cpp, the production handlers live in
// App::register_rpc_methods() and capture an `App` (which needs SDL +
// ImGui and cannot run headlessly).  This file wires the *same* handler
// logic onto a real rpc::Dispatcher and a real AppController, then
// drives requests through Dispatcher::handle_request() exactly as
// RpcServer::drain() does in production.  The handler bodies below are
// kept equivalent to app_rpc_methods.cpp; if that file changes, mirror
// the change here.
//
// The controller methods themselves (select_group, select_range) are
// unit-tested in test_app_controller.cpp.  These tests focus on the RPC
// layer: param validation, the JSON-RPC envelope, and the response
// shape that external clients consume.

#include <catch2/catch_test_macros.hpp>

#include "app/controller.h"
#include "app/app.h"             // ImageEntry
#include "app/io/texture_uploader.h"
#include "app/status_reporter.h"
#include "app/rpc/rpc_dispatcher.h"
#include "core/media_source.h"   // complete type for ImageEntry's unique_ptr
#include "domain/diff_service.h"
#include "domain/image_library.h"
#include "domain/selection_model.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <set>
#include <string>

using idiff::rpc::Dispatcher;
using idiff::rpc::ErrorCode;
using idiff::rpc::RpcException;
using nlohmann::json;

namespace {

class CountingUploader : public idiff::ITextureUploader {
public:
    SDL_Texture* upload(const idiff::UploadRequest&) override {
        return reinterpret_cast<SDL_Texture*>(
            static_cast<std::uintptr_t>(++next_cookie_));
    }
    void destroy(SDL_Texture*) override { ++destroy_count_; }

private:
    std::uintptr_t next_cookie_ = 0;
    int destroy_count_ = 0;
};

class SilentStatusReporter : public idiff::IStatusReporter {
public:
    void set_status(const std::string&) override {}
    void append_status(const std::string&) override {}
    void set_sr_status(const std::string&) override {}
    void show_error(const std::string&, const std::string&) override {}
};

idiff::ImageEntry make_entry(const std::string& path,
                             const std::string& filename) {
    idiff::ImageEntry e;
    e.path = path;
    e.filename = filename;
    e.display_label = filename;
    return e;
}

// Shared param helpers, mirroring app_rpc_methods.cpp.
const json& require_object(const json& params) {
    if (!params.is_object())
        throw RpcException(ErrorCode::InvalidParams,
                           "params must be a JSON object");
    return params;
}

int require_int_field(const json& params, const char* key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_number_integer())
        throw RpcException(ErrorCode::InvalidParams,
            std::string("missing or non-integer field: ") + key);
    return it->get<int>();
}

void check_index(int idx, std::size_t size, const char* what) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= size)
        throw RpcException(ErrorCode::InvalidParams,
            std::string(what) + " out of range");
}

// Test rig: a real AppController + a real Dispatcher with the two
// group-aware selection handlers registered.  The handler bodies
// mirror app_rpc_methods.cpp.
class SelectionRig {
public:
    SelectionRig() : controller_(uploader_, reporter_) {
        register_methods();
    }

    idiff::AppController& controller() { return controller_; }

    json call(const std::string& method, const json& params) {
        json req = {{"jsonrpc", "2.0"}, {"method", method}, {"id", 1}};
        if (!params.is_null()) req["params"] = params;
        std::string out = dispatcher_.handle_request(req.dump());
        REQUIRE_FALSE(out.empty());
        return json::parse(out);
    }

private:
    void register_methods() {
        auto& d = dispatcher_;

        // --- selection.select_group (mirrors app_rpc_methods.cpp) --
        d.register_method("selection.select_group",
            [this](const json& params) -> json {
                require_object(params);
                int idx = require_int_field(params, "index");
                check_index(idx, controller_.library().all().size(), "index");
                bool changed = controller_.select_group(idx);
                json indices = json::array();
                for (int s : controller_.selection().indices())
                    indices.push_back(s);
                return json{{"changed", changed},
                            {"indices", std::move(indices)}};
            });

        // --- selection.select_range (mirrors app_rpc_methods.cpp) --
        d.register_method("selection.select_range",
            [this](const json& params) -> json {
                require_object(params);
                int from = require_int_field(params, "from");
                int to   = require_int_field(params, "to");
                check_index(from, controller_.library().all().size(), "from");
                check_index(to,   controller_.library().all().size(), "to");
                bool changed = controller_.select_range(from, to);
                json indices = json::array();
                for (int s : controller_.selection().indices())
                    indices.push_back(s);
                return json{{"changed", changed},
                            {"indices", std::move(indices)}};
            });
    }

    CountingUploader uploader_;
    SilentStatusReporter reporter_;
    idiff::AppController controller_;
    Dispatcher dispatcher_;
};

// Two comparisons by filename stem: "a" (indices 0,1) and "b"
// (indices 2,3).
void load_two_comparisons(idiff::AppController& c) {
    c.library().add(make_entry("/Foo/a.png", "a.png"));
    c.library().add(make_entry("/Bar/a.png", "a.png"));
    c.library().add(make_entry("/Foo/b.png", "b.png"));
    c.library().add(make_entry("/Bar/b.png", "b.png"));
}

std::set<int> indices_of(const json& result) {
    std::set<int> s;
    for (const auto& v : result["indices"]) s.insert(v.get<int>());
    return s;
}

} // namespace

TEST_CASE("RPC selection.select_group selects the clicked entry's group",
          "[rpc][selection]") {
    SelectionRig rig;
    load_two_comparisons(rig.controller());

    // Index 2 belongs to comparison "b" (indices 2,3).
    json resp = rig.call("selection.select_group", json{{"index", 2}});
    REQUIRE(resp.contains("result"));
    REQUIRE(resp["result"]["changed"] == true);
    REQUIRE(indices_of(resp["result"]) == std::set<int>{2, 3});
    REQUIRE(rig.controller().selection().indices() == std::set<int>{2, 3});
}

TEST_CASE("RPC selection.select_group reports changed=false on a no-op",
          "[rpc][selection]") {
    SelectionRig rig;
    load_two_comparisons(rig.controller());

    // First call selects comparison "a" (0,1).
    REQUIRE(rig.call("selection.select_group",
                     json{{"index", 0}})["result"]["changed"] == true);

    // Re-selecting the same group does not change the selection.
    json resp = rig.call("selection.select_group", json{{"index", 1}});
    REQUIRE(resp["result"]["changed"] == false);
    REQUIRE(indices_of(resp["result"]) == std::set<int>{0, 1});
}

TEST_CASE("RPC selection.select_group rejects an out-of-range index",
          "[rpc][selection]") {
    SelectionRig rig;
    load_two_comparisons(rig.controller());

    json resp = rig.call("selection.select_group", json{{"index", 9}});
    REQUIRE(resp.contains("error"));
    REQUIRE(resp["error"]["code"] ==
            static_cast<int>(ErrorCode::InvalidParams));
    REQUIRE(rig.controller().selection().indices().empty());
}

TEST_CASE("RPC selection.select_group rejects a missing index field",
          "[rpc][selection]") {
    SelectionRig rig;
    load_two_comparisons(rig.controller());

    json resp = rig.call("selection.select_group", json::object());
    REQUIRE(resp.contains("error"));
    REQUIRE(resp["error"]["code"] ==
            static_cast<int>(ErrorCode::InvalidParams));
}

TEST_CASE("RPC selection.select_range selects the inclusive range",
          "[rpc][selection]") {
    SelectionRig rig;
    load_two_comparisons(rig.controller());

    json resp = rig.call("selection.select_range",
                         json{{"from", 1}, {"to", 3}});
    REQUIRE(resp.contains("result"));
    REQUIRE(resp["result"]["changed"] == true);
    REQUIRE(indices_of(resp["result"]) == std::set<int>{1, 2, 3});
    REQUIRE(rig.controller().selection().indices() ==
            std::set<int>{1, 2, 3});
}

TEST_CASE("RPC selection.select_range rejects an out-of-range endpoint",
          "[rpc][selection]") {
    SelectionRig rig;
    load_two_comparisons(rig.controller());

    json resp = rig.call("selection.select_range",
                         json{{"from", 0}, {"to", 99}});
    REQUIRE(resp.contains("error"));
    REQUIRE(resp["error"]["code"] ==
            static_cast<int>(ErrorCode::InvalidParams));
    REQUIRE(rig.controller().selection().indices().empty());
}

TEST_CASE("RPC selection.select_range rejects a missing endpoint field",
          "[rpc][selection]") {
    SelectionRig rig;
    load_two_comparisons(rig.controller());

    json resp = rig.call("selection.select_range", json{{"from", 0}});
    REQUIRE(resp.contains("error"));
    REQUIRE(resp["error"]["code"] ==
            static_cast<int>(ErrorCode::InvalidParams));
}
