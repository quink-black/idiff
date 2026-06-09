// JSON-RPC 2.0 protocol layer for idiff_rpc.
//
// This translation unit is deliberately free of:
//   * sockets / Asio,
//   * App / AppController,
//   * threading primitives.
//
// The Dispatcher only knows how to:
//   * parse one request frame (single object OR batch array),
//   * route a request to a registered handler keyed by `method`,
//   * format JSON-RPC 2.0 success / error responses,
//   * suppress responses for notifications (requests with no `id`).
//
// Every method handler runs in the caller's thread; in the production
// configuration, RpcServer::drain() invokes this dispatcher on the main
// (GUI) thread between ImGui frames, so handlers may freely touch App
// state without further synchronisation.  See Phase 1 plan section C.

#ifndef IDIFF_RPC_DISPATCHER_H
#define IDIFF_RPC_DISPATCHER_H

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace idiff::rpc {

// JSON-RPC 2.0 standard error codes (https://www.jsonrpc.org/specification#error_object).
// `ApplicationError` and the rest of the -32000..-32099 band are reserved
// for application-level failures (file not found, viewport empty, etc.).
enum class ErrorCode : int {
    ParseError       = -32700,
    InvalidRequest   = -32600,
    MethodNotFound   = -32601,
    InvalidParams    = -32602,
    InternalError    = -32603,
    ApplicationError = -32000,
};

// Thrown by method handlers to signal a structured failure that the
// dispatcher will translate into a JSON-RPC `error` response.  Plain
// std::exception (or any other unrelated throw) is caught at the
// dispatcher boundary and reported as InternalError, so handlers
// should prefer this type whenever they want to communicate a
// specific code or carry structured `data`.
class RpcException : public std::runtime_error {
public:
    RpcException(ErrorCode code,
                 std::string msg,
                 nlohmann::json data = nullptr)
        : std::runtime_error(msg)
        , code_(code)
        , msg_(std::move(msg))
        , data_(std::move(data)) {}

    ErrorCode code() const noexcept { return code_; }
    const std::string& msg() const noexcept { return msg_; }
    const nlohmann::json& data() const noexcept { return data_; }

private:
    ErrorCode code_;
    std::string msg_;
    nlohmann::json data_;
};

// A method handler receives the already-validated `params` payload
// (object, array, or null) and returns the `result` to embed in the
// success response.  Throw RpcException to produce a structured error
// response; any other exception is reported as InternalError.
using MethodHandler =
    std::function<nlohmann::json(const nlohmann::json& params)>;

class Dispatcher {
public:
    Dispatcher() = default;

    // Register or replace a handler for `name`.  Names follow the
    // dot.separated convention used in the Phase 1 protocol
    // (`state.get`, `library.load`, ...).  Replacing an existing
    // handler is allowed; newer registrations win.
    void register_method(std::string name, MethodHandler handler);

    // Returns true if a method with this name is currently registered.
    // Useful for tests and dispatcher introspection.
    bool has_method(const std::string& name) const noexcept;

    // Top-level entry point.  Accepts the raw JSON text of a single
    // request OR a batch (a non-empty JSON array of requests, per
    // JSON-RPC 2.0).  Returns the JSON text of the response, or the
    // empty string if the entire request consisted of notifications
    // (in which case the JSON-RPC spec requires no response at all).
    //
    // This method never throws.  Any exception produced by a handler
    // is converted into a JSON-RPC error response.
    std::string handle_request(const std::string& request_json);

private:
    // Process one request object (the unit case).  The boolean
    // out-parameter `is_notification` is set so the batch path can
    // suppress notification responses without re-parsing.
    nlohmann::json handle_one(const nlohmann::json& request,
                              bool& is_notification) noexcept;

    std::unordered_map<std::string, MethodHandler> methods_;
};

// Helper: format a JSON-RPC error response object.  Exposed so the
// transport layer (rpc_server.cpp) can produce framing-level errors
// (oversized frame, bad UTF-8) using the same envelope shape.
nlohmann::json make_error_response(ErrorCode code,
                                   const std::string& message,
                                   const nlohmann::json& id = nullptr,
                                   const nlohmann::json& data = nullptr);

} // namespace idiff::rpc
#endif
