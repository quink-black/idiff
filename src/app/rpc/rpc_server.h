// JSON-RPC 2.0 transport: Unix Domain Socket server.
//
// Phase 1 architecture (single-state multi-channel):
//
//   GUI thread (main)                      Asio I/O thread
//   ----------------                       ---------------
//   App::frame() loop                      io_context::run()
//      |                                       |
//      | RpcServer::drain()                    | accept / read frames
//      v                                       v
//   pop pending request -----promise<-----  push (req_json, promise)
//   Dispatcher::handle_request                 |  await future
//   set promise value -----future----------->  | write framed response
//
// Invariants:
//   * The Asio thread NEVER touches App / AppController / SDL / OpenCV.
//     It only owns sockets, framing buffers, and the request queue.
//   * drain() is the ONLY place Dispatcher handlers run, and it is
//     called from the main thread.  Therefore handlers may freely
//     access App state without further synchronisation.
//   * Wire format: 4-byte big-endian length prefix, followed by that
//     many bytes of UTF-8 JSON.  64 MiB max per frame; oversize frames
//     close the connection (the per-connection buffer would otherwise
//     be a DoS vector against a buggy or hostile local client).
//   * Notifications: handle_request returns "" -> we write nothing,
//     keep the connection open.
//
// PIMPL: all Asio types are confined to rpc_server.cpp.  Consumers
// (App, tests) only see std::string socket paths and a Dispatcher
// reference.

#ifndef IDIFF_RPC_SERVER_H
#define IDIFF_RPC_SERVER_H

#include <cstddef>
#include <memory>
#include <string>

namespace idiff::rpc {

class Dispatcher;

// Maximum bytes accepted in a single framed request.  64 MiB is well
// above realistic state.get / view.screenshot payloads, but keeps a
// hostile or buggy client from forcing a multi-GiB allocation via a
// crafted length prefix.
inline constexpr std::size_t kMaxFrameBytes = 64ull * 1024 * 1024;

class RpcServer {
public:
    // Build a server bound to `socket_path` that will dispatch through
    // `dispatcher`.  The Dispatcher must outlive this RpcServer (the
    // typical owner is App, which owns both).
    //
    // Construction does not open the socket; call start().
    RpcServer(std::string socket_path, Dispatcher& dispatcher);

    // Stops the server if running.
    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    // Bind, listen, and spawn the I/O thread.  If the socket path
    // already exists it is unlinked first (typical for a stale path
    // from a previous crashed instance).
    //
    // Throws std::system_error on bind/listen failure.  Idempotent if
    // already running (no-op).
    void start();

    // Stop accepting new connections, close existing sessions, and
    // join the I/O thread.  Safe to call from the main thread; the
    // destructor calls this automatically.
    void stop();

    // Process all currently queued requests on the calling thread.
    // Intended to be called once per GUI frame.  Each pending request
    // is dispatched synchronously; the response is delivered back to
    // the originating Asio session via its waiting promise.
    //
    // Returns the number of requests dispatched in this call.  The
    // method does not block: if the queue is empty it returns 0.
    std::size_t drain();

    // Path the server is (or will be) bound to.  Useful for tests
    // and for logging the actual socket location at startup.
    const std::string& socket_path() const noexcept;

    // True once start() has succeeded and the I/O thread is running.
    bool is_running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace idiff::rpc

#endif
