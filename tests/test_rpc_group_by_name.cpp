// Integration tests for the group-by-name RPC surface:
//
//   * selection.set rejects cross-comparison selections while
//     group-by-name is on, and allows them while it is off.
//   * view.set_group_by_name flips the flag (and validates its param).
//   * state.get reports the current group_by_name flag.
//
// What this covers and what it does not
// -------------------------------------
// The production handlers live in App::register_rpc_methods()
// (src/app/app_rpc_methods.cpp).  Those closures capture an `App`,
// which needs SDL + ImGui and cannot be brought up headlessly, so the
// test executable cannot register the literal App handlers.  Instead
// this file wires the *same* handler logic onto a real rpc::Dispatcher
// and a real AppController, then drives requests through
// Dispatcher::handle_request() exactly as RpcServer::drain() does in
// production.  This exercises the real protocol envelope (JSON-RPC 2.0
// parse / route / error formatting) and the real controller contracts
// the handlers depend on (comparison_key_of, selection replace,
// apply_comparison_reference).  The handler bodies below are kept
// byte-for-byte equivalent to app_rpc_methods.cpp; if that file
// changes, mirror the change here.
//
// The group_by_name flag lives on App (App::state_->group_by_name) in
// production; here a local bool stands in for it, matching the
// rpc_group_by_name() / rpc_set_group_by_name() accessors.

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

// Test rig: a real AppController + a real Dispatcher with the three
// group-by-name handlers registered.  The handler bodies mirror
// app_rpc_methods.cpp; the only substitution is a local
// `group_by_name` bool standing in for App::state_->group_by_name.
class GroupByNameRig {
public:
    GroupByNameRig() : controller_(uploader_, reporter_) {
        register_methods();
    }

    idiff::AppController& controller() { return controller_; }
    bool group_by_name() const { return group_by_name_; }

    // Drive one request and return the parsed JSON-RPC response.
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

        // --- selection.set (mirrors app_rpc_methods.cpp) -----------
        d.register_method("selection.set",
            [this](const json& params) -> json {
                if (!params.is_object())
                    throw RpcException(ErrorCode::InvalidParams,
                                       "params must be a JSON object");
                auto it = params.find("indices");
                if (it == params.end() || !it->is_array())
                    throw RpcException(ErrorCode::InvalidParams,
                        "missing or non-array field: indices");
                std::set<int> new_sel;
                const std::size_t n = controller_.library().all().size();
                for (const auto& v : *it) {
                    if (!v.is_number_integer())
                        throw RpcException(ErrorCode::InvalidParams,
                            "indices[] must contain integers only");
                    int idx = v.get<int>();
                    if (idx < 0 || static_cast<std::size_t>(idx) >= n)
                        throw RpcException(ErrorCode::InvalidParams,
                            "indices[i] out of range");
                    new_sel.insert(idx);
                }

                if (group_by_name_ && new_sel.size() >= 2) {
                    int anchor = *new_sel.begin();
                    std::string anchor_key =
                        controller_.comparison_key_of(anchor);
                    if (!anchor_key.empty()) {
                        for (int idx : new_sel) {
                            std::string k =
                                controller_.comparison_key_of(idx);
                            if (!k.empty() && k != anchor_key) {
                                throw RpcException(ErrorCode::InvalidParams,
                                    "selection spans multiple comparisons "
                                    "while group-by-name is on");
                            }
                        }
                    }
                }

                if (controller_.selection().replace(std::move(new_sel))) {
                    controller_.diff().mark_dirty();
                    controller_.apply_comparison_reference();
                }
                return json::object();
            });

        // --- view.set_group_by_name (mirrors app_rpc_methods.cpp) --
        d.register_method("view.set_group_by_name",
            [this](const json& params) -> json {
                if (!params.is_object())
                    throw RpcException(ErrorCode::InvalidParams,
                                       "params must be a JSON object");
                auto it = params.find("enabled");
                if (it == params.end() || !it->is_boolean())
                    throw RpcException(ErrorCode::InvalidParams,
                        "missing or non-boolean field: enabled");
                group_by_name_ = it->get<bool>();
                return json::object();
            });

        // --- state.get (group_by_name field only) ------------------
        d.register_method("state.get",
            [this](const json&) -> json {
                json selection = json::array();
                for (int s : controller_.selection().indices())
                    selection.push_back(s);
                return json{
                    {"group_by_name", group_by_name_},
                    {"selection",     std::move(selection)},
                };
            });
    }

    CountingUploader uploader_;
    SilentStatusReporter reporter_;
    idiff::AppController controller_;
    Dispatcher dispatcher_;
    bool group_by_name_ = true;  // matches AppSettings default
};

// Two comparisons by filename stem: "a" (indices 0,1) and "b"
// (indices 2,3).
void load_two_comparisons(idiff::AppController& c) {
    c.library().add(make_entry("/Foo/a.png", "a.png"));
    c.library().add(make_entry("/Bar/a.png", "a.png"));
    c.library().add(make_entry("/Foo/b.png", "b.png"));
    c.library().add(make_entry("/Bar/b.png", "b.png"));
}

} // namespace

TEST_CASE("RPC selection.set rejects cross-comparison selection when "
          "group-by-name is on",
          "[rpc][group_by_name]") {
    GroupByNameRig rig;
    load_two_comparisons(rig.controller());
    REQUIRE(rig.group_by_name());

    // Index 1 (file:a) + index 2 (file:b) span two comparisons.
    json resp = rig.call("selection.set", json{{"indices", {1, 2}}});

    REQUIRE(resp.contains("error"));
    REQUIRE(resp["error"]["code"] ==
            static_cast<int>(ErrorCode::InvalidParams));

    // The selection must not have been partially applied.
    REQUIRE(rig.controller().selection().indices().empty());
}

TEST_CASE("RPC selection.set allows a same-comparison selection when "
          "group-by-name is on",
          "[rpc][group_by_name]") {
    GroupByNameRig rig;
    load_two_comparisons(rig.controller());

    // Indices 2 and 3 are both file:b.
    json resp = rig.call("selection.set", json{{"indices", {2, 3}}});

    REQUIRE(resp.contains("result"));
    REQUIRE(rig.controller().selection().indices() == std::set<int>{2, 3});
}

TEST_CASE("RPC selection.set allows cross-comparison selection when "
          "group-by-name is off",
          "[rpc][group_by_name]") {
    GroupByNameRig rig;
    load_two_comparisons(rig.controller());

    // Disable grouping, then a cross-comparison selection is allowed.
    REQUIRE(rig.call("view.set_group_by_name",
                     json{{"enabled", false}}).contains("result"));
    REQUIRE_FALSE(rig.group_by_name());

    json resp = rig.call("selection.set", json{{"indices", {0, 3}}});
    REQUIRE(resp.contains("result"));
    REQUIRE(rig.controller().selection().indices() == std::set<int>{0, 3});
}

TEST_CASE("RPC selection.set permits a single-index selection regardless "
          "of grouping",
          "[rpc][group_by_name]") {
    GroupByNameRig rig;
    load_two_comparisons(rig.controller());

    // A single index can never span comparisons, so the guard never
    // fires even with grouping on.
    json resp = rig.call("selection.set", json{{"indices", {3}}});
    REQUIRE(resp.contains("result"));
    REQUIRE(rig.controller().selection().indices() == std::set<int>{3});
}

TEST_CASE("RPC view.set_group_by_name toggles the flag and is reflected "
          "in state.get",
          "[rpc][group_by_name]") {
    GroupByNameRig rig;

    json st = rig.call("state.get", json(nullptr));
    REQUIRE(st["result"]["group_by_name"] == true);

    REQUIRE(rig.call("view.set_group_by_name",
                     json{{"enabled", false}}).contains("result"));
    st = rig.call("state.get", json(nullptr));
    REQUIRE(st["result"]["group_by_name"] == false);

    REQUIRE(rig.call("view.set_group_by_name",
                     json{{"enabled", true}}).contains("result"));
    st = rig.call("state.get", json(nullptr));
    REQUIRE(st["result"]["group_by_name"] == true);
}

TEST_CASE("RPC view.set_group_by_name rejects a non-boolean enabled field",
          "[rpc][group_by_name]") {
    GroupByNameRig rig;

    json resp = rig.call("view.set_group_by_name",
                         json{{"enabled", "yes"}});
    REQUIRE(resp.contains("error"));
    REQUIRE(resp["error"]["code"] ==
            static_cast<int>(ErrorCode::InvalidParams));

    // The flag is unchanged (still the default-on).
    REQUIRE(rig.group_by_name());
}

TEST_CASE("RPC view.set_group_by_name rejects a missing enabled field",
          "[rpc][group_by_name]") {
    GroupByNameRig rig;

    json resp = rig.call("view.set_group_by_name", json::object());
    REQUIRE(resp.contains("error"));
    REQUIRE(resp["error"]["code"] ==
            static_cast<int>(ErrorCode::InvalidParams));
}
