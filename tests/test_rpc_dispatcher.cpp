// Unit tests for idiff::rpc::Dispatcher.
//
// The dispatcher is the JSON-RPC 2.0 protocol layer; it has no socket
// or App dependencies, so these tests run in pure-function isolation.
// We register stub handlers that return the params back, throw
// RpcException with a chosen code, or throw a generic std::exception,
// then assert the resulting wire-level response shape.
//
// Coverage:
//   * Valid request -> success response carries `result` and the
//     original `id`.
//   * Notification (no `id`) -> empty response, but handler still runs.
//   * Unknown method -> -32601 MethodNotFound.
//   * Malformed JSON -> -32700 ParseError, id == null.
//   * Missing / wrong jsonrpc -> -32600 InvalidRequest.
//   * Bad params shape -> -32602 InvalidParams.
//   * Handler throws RpcException(InvalidParams) -> -32602.
//   * Handler throws std::runtime_error -> -32603 InternalError.
//   * Batch with mixed requests + notifications -> only requests in
//     response array.
//   * Empty batch -> -32600 InvalidRequest.
//   * Batch of all notifications -> empty response.

#include "app/rpc/rpc_dispatcher.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

using idiff::rpc::Dispatcher;
using idiff::rpc::ErrorCode;
using idiff::rpc::RpcException;
using nlohmann::json;

namespace {

// Build a Dispatcher pre-loaded with a few simple handlers.  Helps
// keep individual test cases short.
Dispatcher make_test_dispatcher(int& notification_call_count) {
    Dispatcher d;
    // Echoes its params back.
    d.register_method("echo",
        [](const json& params) -> json { return params; });

    // Returns a fixed integer regardless of params.
    d.register_method("answer",
        [](const json&) -> json { return 42; });

    // Throws an InvalidParams RpcException so we can assert the
    // dispatcher maps it through.
    d.register_method("bad_params",
        [](const json&) -> json {
            throw RpcException(ErrorCode::InvalidParams,
                               "expected positive integer");
        });

    // Throws an unrelated std::exception to verify InternalError fallback.
    d.register_method("boom",
        [](const json&) -> json {
            throw std::runtime_error("kaboom");
        });

    // Side-effect-only handler used to verify notifications still run.
    d.register_method("ping",
        [&notification_call_count](const json&) -> json {
            ++notification_call_count;
            return json{{"pong", true}};
        });

    return d;
}

} // namespace

TEST_CASE("Dispatcher: valid request returns success envelope",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req =
        R"({"jsonrpc":"2.0","method":"answer","id":7})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["jsonrpc"] == "2.0");
    REQUIRE(resp["id"] == 7);
    REQUIRE(resp.contains("result"));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"] == 42);
}

TEST_CASE("Dispatcher: echo handler receives params",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req =
        R"({"jsonrpc":"2.0","method":"echo",
            "params":{"x":1,"y":[2,3]},"id":"req-1"})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["id"] == "req-1");
    REQUIRE(resp["result"]["x"] == 1);
    REQUIRE(resp["result"]["y"] == json::array({2, 3}));
}

TEST_CASE("Dispatcher: notification produces empty response but still runs",
          "[rpc][dispatcher]") {
    int call_count = 0;
    Dispatcher d = make_test_dispatcher(call_count);

    // No `id` field -> notification.
    const std::string req =
        R"({"jsonrpc":"2.0","method":"ping"})";
    std::string resp = d.handle_request(req);

    REQUIRE(resp.empty());
    REQUIRE(call_count == 1);
}

TEST_CASE("Dispatcher: unknown method -> MethodNotFound (-32601)",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req =
        R"({"jsonrpc":"2.0","method":"no.such.method","id":1})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE_FALSE(resp.contains("result"));
    REQUIRE(resp["error"]["code"] == -32601);
    REQUIRE(resp["id"] == 1);
}

TEST_CASE("Dispatcher: malformed JSON -> ParseError (-32700) with null id",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req = R"({"jsonrpc":"2.0","method":)";  // truncated
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["error"]["code"] == -32700);
    REQUIRE(resp["id"].is_null());
}

TEST_CASE("Dispatcher: missing jsonrpc version -> InvalidRequest (-32600)",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req = R"({"method":"answer","id":1})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["error"]["code"] == -32600);
    REQUIRE(resp["id"] == 1);
}

TEST_CASE("Dispatcher: wrong jsonrpc version -> InvalidRequest",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req =
        R"({"jsonrpc":"1.0","method":"answer","id":1})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["error"]["code"] == -32600);
}

TEST_CASE("Dispatcher: missing method field -> InvalidRequest",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req = R"({"jsonrpc":"2.0","id":1})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["error"]["code"] == -32600);
}

TEST_CASE("Dispatcher: params is a scalar -> InvalidParams (-32602)",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req =
        R"({"jsonrpc":"2.0","method":"echo","params":42,"id":1})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["error"]["code"] == -32602);
}

TEST_CASE("Dispatcher: handler RpcException maps to its declared code",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req =
        R"({"jsonrpc":"2.0","method":"bad_params","id":2})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["error"]["code"] == -32602);
    REQUIRE(resp["error"]["message"]
                .get<std::string>().find("positive integer")
            != std::string::npos);
    REQUIRE(resp["id"] == 2);
}

TEST_CASE("Dispatcher: generic std::exception maps to InternalError (-32603)",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req =
        R"({"jsonrpc":"2.0","method":"boom","id":3})";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp["error"]["code"] == -32603);
    REQUIRE(resp["error"]["message"]
                .get<std::string>().find("kaboom")
            != std::string::npos);
}

TEST_CASE("Dispatcher: batch with mixed requests + notifications",
          "[rpc][dispatcher]") {
    int call_count = 0;
    Dispatcher d = make_test_dispatcher(call_count);

    // First entry has id (request); second has no id (notification);
    // third has id (request).  Response array should contain only the
    // first and third.
    const std::string req = R"([
        {"jsonrpc":"2.0","method":"answer","id":1},
        {"jsonrpc":"2.0","method":"ping"},
        {"jsonrpc":"2.0","method":"echo","params":[],"id":2}
    ])";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp.is_array());
    REQUIRE(resp.size() == 2);
    REQUIRE(resp[0]["id"] == 1);
    REQUIRE(resp[1]["id"] == 2);
    REQUIRE(call_count == 1);  // notification still ran
}

TEST_CASE("Dispatcher: empty batch -> InvalidRequest",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    const std::string req = "[]";
    json resp = json::parse(d.handle_request(req));

    REQUIRE(resp.is_object());
    REQUIRE(resp["error"]["code"] == -32600);
    REQUIRE(resp["id"].is_null());
}

TEST_CASE("Dispatcher: batch of all notifications -> empty response",
          "[rpc][dispatcher]") {
    int call_count = 0;
    Dispatcher d = make_test_dispatcher(call_count);

    const std::string req = R"([
        {"jsonrpc":"2.0","method":"ping"},
        {"jsonrpc":"2.0","method":"ping"}
    ])";
    std::string resp = d.handle_request(req);

    REQUIRE(resp.empty());
    REQUIRE(call_count == 2);
}

TEST_CASE("Dispatcher: top-level scalar JSON -> InvalidRequest",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    json resp = json::parse(d.handle_request("42"));

    REQUIRE(resp["error"]["code"] == -32600);
    REQUIRE(resp["id"].is_null());
}

TEST_CASE("Dispatcher: id can be string or number; null is allowed",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    {
        json resp = json::parse(d.handle_request(
            R"({"jsonrpc":"2.0","method":"answer","id":"abc"})"));
        REQUIRE(resp["id"] == "abc");
        REQUIRE(resp["result"] == 42);
    }
    {
        // id = null is a valid request (NOT a notification).  We should
        // get a response with id == null.
        json resp = json::parse(d.handle_request(
            R"({"jsonrpc":"2.0","method":"answer","id":null})"));
        REQUIRE(resp["id"].is_null());
        REQUIRE(resp.contains("result"));
    }
}

TEST_CASE("Dispatcher: has_method reflects registrations",
          "[rpc][dispatcher]") {
    int unused = 0;
    Dispatcher d = make_test_dispatcher(unused);

    REQUIRE(d.has_method("echo"));
    REQUIRE(d.has_method("answer"));
    REQUIRE_FALSE(d.has_method("not_registered"));
}
