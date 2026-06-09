// JSON-RPC 2.0 dispatcher implementation.  See header for contract.
//
// Design choices worth flagging:
//
//   * `id` is preserved verbatim from the request when forming the
//     response.  JSON-RPC permits string / number / null IDs; we don't
//     try to canonicalise them.
//
//   * On parse failure we follow the spec's recommendation to use
//     `"id": null` because the request was unparseable and there is
//     no original ID to echo.
//
//   * Requests with no `id` field are treated as notifications: their
//     handler still runs, but we discard the result and return an
//     empty string so the transport doesn't write a response back.
//     If a notification triggers an exception, the spec says we must
//     suppress the error too — handlers that need at-least-once
//     semantics should not be invoked as notifications.
//
//   * Batch requests follow the spec verbatim: an empty array is an
//     InvalidRequest, a non-empty array is processed element-by-element
//     and the responses concatenated; if every element was a
//     notification, the whole batch produces an empty response.

#include "app/rpc/rpc_dispatcher.h"

#include "util/logger.h"

#include <utility>

namespace idiff::rpc {

using nlohmann::json;

namespace {

// Local sentinel used to distinguish "request had no id field" from
// "request had id: null" when forming responses.  Notifications must
// produce no response at all; null-id requests produce a response with
// id == null.
struct IdSlot {
    bool present = false;
    json value;     // valid only when `present` is true
};

IdSlot extract_id(const json& request) {
    IdSlot slot;
    auto it = request.find("id");
    if (it != request.end()) {
        slot.present = true;
        slot.value = *it;
    }
    return slot;
}

bool is_valid_id(const json& id) {
    // Per JSON-RPC 2.0 the id must be a string, a number, or null.
    // (Fractional numbers are technically allowed; we don't restrict.)
    return id.is_null() || id.is_string() || id.is_number();
}

} // namespace

json make_error_response(ErrorCode code,
                         const std::string& message,
                         const json& id,
                         const json& data) {
    json error_obj = {
        {"code", static_cast<int>(code)},
        {"message", message},
    };
    if (!data.is_null()) {
        error_obj["data"] = data;
    }
    return json{
        {"jsonrpc", "2.0"},
        {"error", std::move(error_obj)},
        {"id", id},
    };
}

void Dispatcher::register_method(std::string name, MethodHandler handler) {
    methods_[std::move(name)] = std::move(handler);
}

bool Dispatcher::has_method(const std::string& name) const noexcept {
    return methods_.find(name) != methods_.end();
}

json Dispatcher::handle_one(const json& request,
                            bool& is_notification) noexcept {
    is_notification = false;

    // 1. Shape validation: must be an object.
    if (!request.is_object()) {
        return make_error_response(
            ErrorCode::InvalidRequest,
            "Request must be a JSON object",
            nullptr);
    }

    // 2. Extract id (may be absent for notifications).
    IdSlot id_slot = extract_id(request);
    if (id_slot.present && !is_valid_id(id_slot.value)) {
        // Spec: an invalid id type is also an InvalidRequest, and the
        // response id must be null.
        return make_error_response(
            ErrorCode::InvalidRequest,
            "Request id must be string, number, or null",
            nullptr);
    }
    is_notification = !id_slot.present;
    json response_id = id_slot.present ? id_slot.value : json(nullptr);

    // 3. jsonrpc version check.
    auto version_it = request.find("jsonrpc");
    if (version_it == request.end() || !version_it->is_string()
            || version_it->get<std::string>() != "2.0") {
        return make_error_response(
            ErrorCode::InvalidRequest,
            "Missing or unsupported jsonrpc version (must be \"2.0\")",
            response_id);
    }

    // 4. method must be a non-empty string.
    auto method_it = request.find("method");
    if (method_it == request.end() || !method_it->is_string()
            || method_it->get<std::string>().empty()) {
        return make_error_response(
            ErrorCode::InvalidRequest,
            "Missing or invalid method name",
            response_id);
    }
    const std::string method_name = method_it->get<std::string>();

    // 5. params: object, array, or absent.  If absent, pass `null` to
    //    the handler so it doesn't have to special-case .find().
    json params = json(nullptr);
    auto params_it = request.find("params");
    if (params_it != request.end()) {
        if (!params_it->is_object() && !params_it->is_array()) {
            return make_error_response(
                ErrorCode::InvalidParams,
                "params must be an object or array when present",
                response_id);
        }
        params = *params_it;
    }

    // 6. Look up handler.
    auto handler_it = methods_.find(method_name);
    if (handler_it == methods_.end()) {
        return make_error_response(
            ErrorCode::MethodNotFound,
            "Method not found: " + method_name,
            response_id);
    }

    // 7. Invoke.  Catch RpcException specifically (carries structured
    //    code + data), then catch any other std::exception and report
    //    it as InternalError so the dispatcher never propagates.
    try {
        json result = handler_it->second(params);
        return json{
            {"jsonrpc", "2.0"},
            {"result", std::move(result)},
            {"id", response_id},
        };
    } catch (const RpcException& ex) {
        return make_error_response(ex.code(), ex.msg(), response_id, ex.data());
    } catch (const std::exception& ex) {
        LOG_ERROR("RPC handler '%s' threw std::exception: %s",
                  method_name.c_str(), ex.what());
        return make_error_response(
            ErrorCode::InternalError,
            std::string("Internal error: ") + ex.what(),
            response_id);
    } catch (...) {
        LOG_ERROR("RPC handler '%s' threw unknown exception",
                  method_name.c_str());
        return make_error_response(
            ErrorCode::InternalError,
            "Internal error: unknown exception",
            response_id);
    }
}

std::string Dispatcher::handle_request(const std::string& request_json) {
    json parsed;
    try {
        parsed = json::parse(request_json);
    } catch (const json::parse_error& ex) {
        json err = make_error_response(
            ErrorCode::ParseError,
            std::string("Parse error: ") + ex.what(),
            nullptr);
        return err.dump();
    }

    // Single request: handle and return the response (or empty for
    // notifications).
    if (parsed.is_object()) {
        bool is_notification = false;
        json response = handle_one(parsed, is_notification);
        if (is_notification) {
            return std::string{};
        }
        return response.dump();
    }

    // Batch request: array of requests.
    if (parsed.is_array()) {
        if (parsed.empty()) {
            json err = make_error_response(
                ErrorCode::InvalidRequest,
                "Batch request must not be empty",
                nullptr);
            return err.dump();
        }
        json batch_response = json::array();
        for (const auto& item : parsed) {
            bool is_notification = false;
            json response = handle_one(item, is_notification);
            if (!is_notification) {
                batch_response.push_back(std::move(response));
            }
        }
        if (batch_response.empty()) {
            // All-notifications batch: spec says no response.
            return std::string{};
        }
        return batch_response.dump();
    }

    // Top-level value is neither object nor array.
    json err = make_error_response(
        ErrorCode::InvalidRequest,
        "Top-level JSON value must be a request object or batch array",
        nullptr);
    return err.dump();
}

} // namespace idiff::rpc
