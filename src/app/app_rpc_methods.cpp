// Phase 1 JSON-RPC method handlers.
//
// Seven methods, each a closure registered against the dispatcher.
// The closures capture `this` (App*) so they can reach App's private
// members directly -- defining them as a member function of App
// (App::register_rpc_methods) keeps that access path explicit.
//
//   state.get             () -> { entries: [...], selection: [...],
//                                 reference: int|null, view: {...} }
//   library.load          (paths: [string]) -> { added: int }
//   library.set_reference (index: int) -> {}
//   library.remove        (index: int) -> {}
//   selection.set         (indices: [int]) -> {}
//   view.set_mode         (mode: "split"|"overlay"|"difference",
//                          slider?: float) -> {}
//   view.screenshot       (path: string,
//                          slider?: float,
//                          mode?: "split"|"overlay"|"difference")
//                            -> { path: string, width: int, height: int }
//
// All handlers run on the main (GUI) thread because that is where
// rpc_server_->drain() is called from frame() -- so they may freely
// mutate App / Controller / SDL state without locks.
//
// Validation: anything that would have been an UI-time error (bad
// index, unknown mode string, missing required field) is reported as
// an InvalidParams (-32602) RpcException.  Anything that is well-
// formed but failed for state reasons (e.g. screenshot composer
// returns empty Mat because nothing is selected) is also surfaced as
// InvalidParams; we reserve InternalError for I/O failures we did not
// expect (encode failure, file write failure).

#include "app/app.h"

#ifdef IDIFF_HAVE_RPC

#include "app/controller.h"
#include "app/rpc/rpc_dispatcher.h"
#include "app/rpc/socket_paths.h"
#include "app/screenshot_composer.h"
#include "app/viewport.h"
#include "core/detail/platform_utf8.h"
#include "core/image.h"
#include "core/media_source.h"
#include "domain/diff_service.h"
#include "domain/image_library.h"
#include "domain/selection_model.h"

#include <nlohmann/json.hpp>

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace idiff {

using nlohmann::json;
using rpc::ErrorCode;
using rpc::RpcException;

namespace {

// --- Param helpers ---------------------------------------------------
//
// Each Phase-1 handler validates by hand because the schemas are tiny
// and we want error messages that name the specific field.  These
// helpers keep the boilerplate out of the handler bodies.

const json& require_object(const json& params) {
    if (!params.is_object()) {
        throw RpcException(ErrorCode::InvalidParams,
                           "params must be a JSON object");
    }
    return params;
}

int require_int_field(const json& params, const char* key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_number_integer()) {
        throw RpcException(ErrorCode::InvalidParams,
            std::string("missing or non-integer field: ") + key);
    }
    return it->get<int>();
}

const std::string& require_string_field(const json& params, const char* key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_string()) {
        throw RpcException(ErrorCode::InvalidParams,
            std::string("missing or non-string field: ") + key);
    }
    return it->get_ref<const std::string&>();
}

ComparisonMode parse_mode(const std::string& s) {
    if (s == "split")      return ComparisonMode::Split;
    if (s == "overlay")    return ComparisonMode::Overlay;
    if (s == "difference") return ComparisonMode::Difference;
    throw RpcException(ErrorCode::InvalidParams,
        "mode must be one of: split, overlay, difference (got \""
        + s + "\")");
}

const char* mode_to_string(ComparisonMode m) {
    switch (m) {
        case ComparisonMode::Split:      return "split";
        case ComparisonMode::Overlay:    return "overlay";
        case ComparisonMode::Difference: return "difference";
    }
    return "split";
}

void check_index(int idx, std::size_t size, const char* what) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= size) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "%s out of range: %d (have %zu entries)",
                      what, idx, size);
        throw RpcException(ErrorCode::InvalidParams, buf);
    }
}

} // namespace


void App::register_rpc_methods() {
    if (!rpc_dispatcher_) return;
    auto& d = *rpc_dispatcher_;

    // --- app.identity ----------------------------------------------
    //
    // Single round-trip the MCP server uses to confirm that the
    // socket path it just connected to actually belongs to an idiff
    // (rather than some unrelated UDS server that happens to live in
    // /tmp/idiff-*).  Also lets a multi-instance UI tell the user
    // which window the agent is talking to.
    d.register_method("app.identity",
        [this](const json& /*params*/) -> json {
            return json{
                {"name",   "idiff"},
                {"pid",    rpc_pid()},
                {"socket", rpc_socket_path()},
                {"label",  rpc_identity()},
            };
        });

    // --- app.list_instances ----------------------------------------
    //
    // Re-runs the /tmp/idiff-*.sock sweep on demand and reports back
    // one entry per probe.  Stale entries (ECONNREFUSED) are cleaned
    // up as a side effect of the sweep, mirroring what init() does.
    // Used by the MCP server when its own discovery turns up more
    // than one live instance: returning the list lets the agent
    // show the user a chooser without poking the filesystem itself.
    d.register_method("app.list_instances",
        [this](const json& /*params*/) -> json {
            auto probes = rpc::sweep_stale_sockets();
            json arr = json::array();
            for (const auto& p : probes) {
                json je = {
                    {"path",    p.path},
                    {"pid",     p.pid},
                    {"alive",   p.alive},
                    {"removed", p.removed},
                };
                if (p.alive && p.pid > 0) {
                    je["label"] = rpc::compose_identity_label(p.pid);
                }
                if (p.alive && p.pid == rpc_pid()) {
                    je["self"] = true;
                }
                arr.push_back(std::move(je));
            }
            return json{
                {"self_pid",    rpc_pid()},
                {"self_socket", rpc_socket_path()},
                {"self_label",  rpc_identity()},
                {"instances",   std::move(arr)},
            };
        });

    // --- state.get -------------------------------------------------
    //
    // Returns a snapshot of everything an external client needs to
    // understand the current session: image list, selection, explicit
    // reference (if any), viewport mode + slider, image dimensions.
    // No params.
    d.register_method("state.get",
        [this](const json& /*params*/) -> json {
            json entries_json = json::array();
            const auto& entries = entries_view();
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                json je;
                je["index"]    = static_cast<int>(i);
                je["path"]     = e.path;
                je["filename"] = e.filename;
                je["label"]    = e.display_label;
                if (e.image) {
                    const auto& info = e.image->info();
                    je["width"]  = info.width;
                    je["height"] = info.height;
                } else {
                    je["width"]  = 0;
                    je["height"] = 0;
                }
                je["frames"] = e.source ? e.source->frame_count() : 1;
                entries_json.push_back(std::move(je));
            }

            json selection_json = json::array();
            for (int s : selection_->indices()) selection_json.push_back(s);

            int ref_idx = -1;
            controller_->get_ref_index(ref_idx);

            Viewport& vp = rpc_viewport();
            json view = {
                {"mode",   mode_to_string(vp.mode())},
                {"slider", vp.overlay_slider_pos()},
            };

            return json{
                {"identity", json{
                    {"name",   "idiff"},
                    {"pid",    rpc_pid()},
                    {"socket", rpc_socket_path()},
                    {"label",  rpc_identity()},
                }},
                {"entries",   std::move(entries_json)},
                {"selection", std::move(selection_json)},
                {"reference", ref_idx < 0 ? json(nullptr) : json(ref_idx)},
                {"explicit_reference",
                    selection_->has_explicit_reference()},
                {"view",      std::move(view)},
            };
        });

    // --- library.load ---------------------------------------------
    //
    // Same code path as the GUI's File > Open: routes through
    // App::load_paths() so JSON comparison configs are auto-detected
    // and YUV files queue the parameter dialog (which we deliberately
    // keep -- raw YUV needs metadata, and silently rejecting it would
    // surprise the user more than a dialog popping up).
    d.register_method("library.load",
        [this](const json& params) -> json {
            require_object(params);
            auto it = params.find("paths");
            if (it == params.end() || !it->is_array()) {
                throw RpcException(ErrorCode::InvalidParams,
                    "missing or non-array field: paths");
            }
            std::vector<std::string> paths;
            paths.reserve(it->size());
            for (const auto& p : *it) {
                if (!p.is_string()) {
                    throw RpcException(ErrorCode::InvalidParams,
                        "paths[] must contain strings only");
                }
                paths.push_back(p.get<std::string>());
            }

            const std::size_t before = entries_view().size();
            load_paths(paths);
            const std::size_t after  = entries_view().size();

            return json{
                {"added", static_cast<int>(after - before)},
                {"total", static_cast<int>(after)},
            };
        });

    // --- library.set_reference -------------------------------------
    //
    // Pin a specific entry as the "A" side of all comparisons.
    // Routes through the controller so the same selection bookkeeping
    // (insert into selection if missing, mark diff dirty) the GUI
    // right-click does also fires here.
    d.register_method("library.set_reference",
        [this](const json& params) -> json {
            require_object(params);
            int idx = require_int_field(params, "index");
            check_index(idx, entries_view().size(), "index");
            controller_->mark_as_reference(idx);
            return json::object();
        });

    // --- library.remove --------------------------------------------
    //
    // Drops an entry and patches the selection.  Goes through
    // App::remove_entry so the file watcher is unsubscribed and any
    // pending reload-dialog notification for that path is cleared.
    d.register_method("library.remove",
        [this](const json& params) -> json {
            require_object(params);
            int idx = require_int_field(params, "index");
            check_index(idx, entries_view().size(), "index");
            remove_entry(idx);
            return json::object();
        });

    // --- selection.set ---------------------------------------------
    //
    // Wholesale replace the selection.  Empty array clears it.  Out-
    // of-range indices are rejected up front so the caller gets one
    // clean error rather than a partially-applied selection.
    d.register_method("selection.set",
        [this](const json& params) -> json {
            require_object(params);
            auto it = params.find("indices");
            if (it == params.end() || !it->is_array()) {
                throw RpcException(ErrorCode::InvalidParams,
                    "missing or non-array field: indices");
            }
            std::set<int> new_sel;
            const std::size_t n = entries_view().size();
            for (const auto& v : *it) {
                if (!v.is_number_integer()) {
                    throw RpcException(ErrorCode::InvalidParams,
                        "indices[] must contain integers only");
                }
                int idx = v.get<int>();
                check_index(idx, n, "indices[i]");
                new_sel.insert(idx);
            }
            // Replacing the selection invalidates the diff cache; the
            // controller's mark_as_reference path does that for us, but
            // a bare replace() does not.
            if (selection_->replace(std::move(new_sel))) {
                diff_service_->mark_dirty();
            }
            return json::object();
        });

    // --- view.set_mode ---------------------------------------------
    //
    // mode is mandatory, slider only meaningful in overlay mode and
    // is silently ignored for other modes (kept as state but not
    // visible until the user switches to overlay).
    d.register_method("view.set_mode",
        [this](const json& params) -> json {
            require_object(params);
            ComparisonMode m = parse_mode(require_string_field(params, "mode"));
            Viewport& vp = rpc_viewport();
            vp.set_mode(m);

            auto it = params.find("slider");
            if (it != params.end()) {
                if (!it->is_number()) {
                    throw RpcException(ErrorCode::InvalidParams,
                        "slider must be a number in [0,1]");
                }
                vp.set_overlay_slider_pos(it->get<float>());
            }
            return json::object();
        });

    // --- view.screenshot -------------------------------------------
    //
    // Composes whatever the viewport is currently showing into a
    // single image and writes it to `path`.  Optional slider/mode let
    // the caller momentarily override the GUI state for this snapshot
    // without the user seeing a flicker -- the override is local to
    // the composer call (we don't mutate the viewport for it).
    d.register_method("view.screenshot",
        [this](const json& params) -> json {
            require_object(params);
            const std::string& path = require_string_field(params, "path");

            Viewport& vp = rpc_viewport();
            ComparisonMode mode = vp.mode();
            float slider = vp.overlay_slider_pos();

            if (auto it = params.find("mode"); it != params.end()) {
                if (!it->is_string()) {
                    throw RpcException(ErrorCode::InvalidParams,
                        "mode must be a string");
                }
                mode = parse_mode(it->get<std::string>());
            }
            if (auto it = params.find("slider"); it != params.end()) {
                if (!it->is_number()) {
                    throw RpcException(ErrorCode::InvalidParams,
                        "slider must be a number in [0,1]");
                }
                slider = std::clamp(it->get<float>(), 0.0f, 1.0f);
            }

            int ref_idx = -1;
            controller_->get_ref_index(ref_idx);

            ComposeViewportInput in;
            in.mode = mode;
            in.ref_idx = ref_idx;
            in.selection = &selection_->indices();
            in.entries = &entries_view();
            in.diff = diff_service_;
            in.overlay_slider_pos = slider;
            in.grid_layout = vp.grid_layout();
            in.grid_cols = vp.grid_cols();

            std::string err;
            cv::Mat composed = compose_viewport(in, &err);
            if (composed.empty()) {
                throw RpcException(ErrorCode::InvalidParams,
                                   "screenshot: " + err);
            }

            // Pick the encoder from the file extension; default to PNG
            // when none was given.  Match save_viewport_dialog's flow.
            auto dot = path.rfind('.');
            std::string ext = (dot != std::string::npos) ? path.substr(dot)
                                                         : ".png";
            std::vector<std::uint8_t> buf;
            if (!cv::imencode(ext, composed, buf)) {
                throw RpcException(ErrorCode::InternalError,
                    "screenshot: failed to encode " + ext + " image");
            }
            std::string final_path = path;
            if (dot == std::string::npos) final_path += ".png";
            if (!platform::write_file_binary(final_path, buf.data(),
                                             buf.size())) {
                throw RpcException(ErrorCode::InternalError,
                    "screenshot: failed to write file: " + final_path);
            }

            return json{
                {"path",   final_path},
                {"width",  composed.cols},
                {"height", composed.rows},
                {"bytes",  static_cast<int>(buf.size())},
            };
        });
}

} // namespace idiff

#endif // IDIFF_HAVE_RPC
