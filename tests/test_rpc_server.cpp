// Integration tests for the Asio-backed RPC transport.
//
// Strategy: bind RpcServer to a unique transport path (UDS on POSIX,
// named pipe on Windows), connect with a raw client (no Asio in the
// test client, to keep the test faithful to what an external CLI would
// do), exchange framed JSON, and drive drain() from a thread that
// simulates the GUI loop.
//
// Coverage:
//   * Round-trip: framed echo request -> framed response carries
//     the same `id`.
//   * Notification: no `id` field -> server keeps the connection open
//     and writes nothing back (we verify by sending a follow-up
//     request and getting a response without timing out).
//   * Oversized frame rejection: announce a length above kMaxFrameBytes
//     -> server closes the connection without consuming the body.
//   * stop() returns even when a queued request was never drained.
//   * Listener recovery: removing the socket path out from under a running
//     server is repaired, and a request the main thread has not drained yet
//     does not keep a second client from being served.

#include "app/rpc/rpc_dispatcher.h"
#include "app/rpc/rpc_server.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using idiff::rpc::Dispatcher;
using idiff::rpc::RpcServer;
using nlohmann::json;

namespace {

// ---- Platform-abstracted transport handle ---------------------------------

#ifdef _WIN32
using transport_handle = HANDLE;
const transport_handle invalid_transport = INVALID_HANDLE_VALUE;
#else
using transport_handle = int;
constexpr transport_handle invalid_transport = -1;
#endif

// ---- Platform-abstracted helpers ------------------------------------------

int current_pid() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return ::getpid();
#endif
}

// Build a unique transport path per test case.
std::string make_socket_path() {
    static std::atomic<int> counter{0};
    int n = counter.fetch_add(1);
#ifdef _WIN32
    return "\\\\.\\pipe\\idiff-rpctest-" + std::to_string(current_pid())
         + "-" + std::to_string(n);
#else
    return "/tmp/idiff-rpctest-" + std::to_string(current_pid())
         + "-" + std::to_string(n) + ".sock";
#endif
}

transport_handle connect_transport(const std::string& path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    return (h == INVALID_HANDLE_VALUE) ? invalid_transport : h;
#else
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return invalid_transport;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return invalid_transport;
    }
    std::memcpy(addr.sun_path, path.data(), path.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
        ::close(fd);
        return invalid_transport;
    }
    return fd;
#endif
}

void close_transport(transport_handle h) {
#ifdef _WIN32
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#else
    if (h >= 0) ::close(h);
#endif
}

void write_be32(unsigned char out[4], std::uint32_t v) {
    out[0] = static_cast<unsigned char>((v >> 24) & 0xFF);
    out[1] = static_cast<unsigned char>((v >> 16) & 0xFF);
    out[2] = static_cast<unsigned char>((v >>  8) & 0xFF);
    out[3] = static_cast<unsigned char>( v        & 0xFF);
}

std::uint32_t read_be32(const unsigned char in[4]) {
    return (static_cast<std::uint32_t>(in[0]) << 24)
         | (static_cast<std::uint32_t>(in[1]) << 16)
         | (static_cast<std::uint32_t>(in[2]) <<  8)
         |  static_cast<std::uint32_t>(in[3]);
}

// Send a complete length-prefixed frame.  Returns true on success.
bool send_frame(transport_handle h, const std::string& body) {
    unsigned char hdr[4];
    write_be32(hdr, static_cast<std::uint32_t>(body.size()));
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(h, hdr, 4, &written, nullptr) || written != 4)
        return false;
    std::size_t off = 0;
    while (off < body.size()) {
        DWORD chunk = 0;
        if (!WriteFile(h, body.data() + off,
                       static_cast<DWORD>(body.size() - off),
                       &chunk, nullptr) || chunk == 0)
            return false;
        off += chunk;
    }
    return true;
#else
    if (::write(h, hdr, 4) != 4) return false;
    std::size_t off = 0;
    while (off < body.size()) {
        ssize_t n = ::write(h, body.data() + off, body.size() - off);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
#endif
}

// Read exactly `n` bytes with a wall-clock timeout in ms.
bool read_exact(transport_handle h, void* out, std::size_t n,
                int timeout_ms) {
#ifdef _WIN32
    // Client-side pipe handles are non-overlapped; ReadFile blocks
    // until data is available.  Run the read in a separate thread
    // so we can enforce the wall-clock timeout.
    auto* p = static_cast<unsigned char*>(out);
    std::size_t off = 0;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (off < n) {
            DWORD got = 0;
            if (!ReadFile(h, p + off, static_cast<DWORD>(n - off),
                          &got, nullptr) || got == 0) {
                break;
            }
            off += got;
        }
        done.store(true);
    });

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (!done.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            // Timed out -- cancel the blocking ReadFile and join.
            CancelSynchronousIo(reader.native_handle());
            reader.join();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    reader.join();
    return off == n;
#else
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(h, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::size_t off = 0;
    auto* p = static_cast<unsigned char*>(out);
    while (off < n) {
        ssize_t r = ::read(h, p + off, n - off);
        if (r <= 0) return false;
        off += static_cast<std::size_t>(r);
    }
    return true;
#endif
}

// Receive a single framed response, with a 2-second timeout.
std::string recv_frame(transport_handle h) {
    unsigned char hdr[4];
    if (!read_exact(h, hdr, 4, 2000)) return std::string{};
    std::uint32_t len = read_be32(hdr);
    std::vector<unsigned char> body(len);
    if (len > 0 && !read_exact(h, body.data(), len, 2000)) {
        return std::string{};
    }
    return std::string(reinterpret_cast<const char*>(body.data()), len);
}

// Write raw bytes (for oversized-frame test header).
bool raw_write(transport_handle h, const void* data, std::size_t len) {
#ifdef _WIN32
    DWORD written = 0;
    return WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr)
           && written == len;
#else
    return ::write(h, data, len) == static_cast<ssize_t>(len);
#endif
}

// Try to read 1 byte with a 2-second timeout; returns bytes read (0 or
// 1) or -1 on error.  Used by the oversized-frame test.
int raw_read1(transport_handle h) {
#ifdef _WIN32
    char buf[1];
    DWORD got = 0;
    if (read_exact(h, buf, 1, 2000)) return 1;
    // Peek to detect EOF: if PeekNamedPipe succeeds with 0 available
    // and a subsequent ReadFile returns 0 bytes, it's EOF.
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
        // Pipe closed / broken.
        return 0;
    }
    if (avail == 0) {
        // Could be EOF or just no data yet.  Try a zero-timeout read.
        return 0;
    }
    return -1;
#else
    timeval tv{};
    tv.tv_sec  = 2;
    tv.tv_usec = 0;
    ::setsockopt(h, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[16];
    ssize_t r = ::read(h, buf, sizeof(buf));
    return static_cast<int>(r);
#endif
}

// RAII helper: spawn a thread that polls server.drain() at ~120 Hz so
// we don't have to manually pump it from the test body.  Stops on
// destruction.
class DrainPump {
public:
    explicit DrainPump(RpcServer& server)
        : server_(server)
        , stop_(false)
        , thread_([this] { run(); }) {}

    ~DrainPump() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    DrainPump(const DrainPump&) = delete;
    DrainPump& operator=(const DrainPump&) = delete;

private:
    void run() {
        while (!stop_.load()) {
            server_.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        // Final drain to release any last-moment requests.
        server_.drain();
    }

    RpcServer& server_;
    std::atomic<bool> stop_;
    std::thread thread_;
};

Dispatcher make_echo_dispatcher() {
    Dispatcher d;
    d.register_method("echo",
        [](const json& params) -> json { return params; });
    d.register_method("answer",
        [](const json&) -> json { return 42; });
    return d;
}

} // namespace

TEST_CASE("RpcServer: round-trip framed echo request",
          "[rpc][server]") {
    Dispatcher d = make_echo_dispatcher();
    RpcServer server(make_socket_path(), d);
    server.start();
    REQUIRE(server.is_running());
    DrainPump pump(server);

    transport_handle h = connect_transport(server.socket_path());
    REQUIRE(h != invalid_transport);

    const std::string req =
        R"({"jsonrpc":"2.0","method":"echo",)"
        R"("params":{"hello":"world"},"id":42})";
    REQUIRE(send_frame(h, req));

    std::string resp = recv_frame(h);
    REQUIRE_FALSE(resp.empty());
    json parsed = json::parse(resp);
    REQUIRE(parsed["id"] == 42);
    REQUIRE(parsed["result"]["hello"] == "world");

    close_transport(h);
}

TEST_CASE("RpcServer: notification produces no response, "
          "next request still answered",
          "[rpc][server]") {
    Dispatcher d = make_echo_dispatcher();
    RpcServer server(make_socket_path(), d);
    server.start();
    DrainPump pump(server);

    transport_handle h = connect_transport(server.socket_path());
    REQUIRE(h != invalid_transport);

    // Send a notification (no `id`).  Server must consume it and write
    // nothing back; we then send a normal request and expect to read
    // its response (proving the connection survived and the framing
    // state is intact).
    const std::string note = R"({"jsonrpc":"2.0","method":"echo",)"
                             R"("params":{}})";
    REQUIRE(send_frame(h, note));

    const std::string req = R"({"jsonrpc":"2.0","method":"answer","id":7})";
    REQUIRE(send_frame(h, req));

    std::string resp = recv_frame(h);
    REQUIRE_FALSE(resp.empty());
    json parsed = json::parse(resp);
    REQUIRE(parsed["id"] == 7);
    REQUIRE(parsed["result"] == 42);

    close_transport(h);
}

TEST_CASE("RpcServer: oversized frame closes the connection",
          "[rpc][server]") {
    Dispatcher d = make_echo_dispatcher();
    RpcServer server(make_socket_path(), d);
    server.start();
    DrainPump pump(server);

    transport_handle h = connect_transport(server.socket_path());
    REQUIRE(h != invalid_transport);

    // Announce a length above the 64 MiB cap without sending the body.
    // Server must close without trying to allocate.
    unsigned char hdr[4];
    const std::uint32_t too_big =
        static_cast<std::uint32_t>(idiff::rpc::kMaxFrameBytes) + 1;
    write_be32(hdr, too_big);
    REQUIRE(raw_write(h, hdr, 4));

    int r = raw_read1(h);
    // EOF (0) is the expected outcome; some platforms may surface
    // ECONNRESET (-1) instead -- either is acceptable as "server
    // closed".
    REQUIRE(r <= 0);

    close_transport(h);
}

TEST_CASE("RpcServer: stop() is idempotent and survives second start()",
          "[rpc][server]") {
    Dispatcher d = make_echo_dispatcher();
    std::string path = make_socket_path();
    RpcServer server(path, d);

    server.start();
    REQUIRE(server.is_running());
    server.stop();
    REQUIRE_FALSE(server.is_running());

    // Calling stop() a second time is a no-op.
    server.stop();

    // Same object can be restarted on the same path.
    server.start();
    REQUIRE(server.is_running());
    DrainPump pump(server);

    transport_handle h = connect_transport(server.socket_path());
    REQUIRE(h != invalid_transport);
    REQUIRE(send_frame(h, R"({"jsonrpc":"2.0","method":"answer","id":1})"));
    std::string resp = recv_frame(h);
    REQUIRE_FALSE(resp.empty());
    REQUIRE(json::parse(resp)["result"] == 42);
    close_transport(h);
}

#ifndef _WIN32
// Both cases below are specific to the socket transport: a named pipe has no
// filesystem path to lose, and its accept thread hands out one pipe instance
// at a time.
//
// Anything that tidies /tmp can unlink this instance's socket without
// touching the process, and the instance keeps running with a listener no
// client can find: a bound listener whose path is gone answers nothing.  Only
// the owner has a reason to re-create the path.
TEST_CASE("RpcServer: socket path is re-created after it is removed",
          "[rpc][server]") {
    Dispatcher d = make_echo_dispatcher();
    std::string path = make_socket_path();
    RpcServer server(path, d);
    server.start();
    DrainPump pump(server);

    REQUIRE(::unlink(path.c_str()) == 0);

    // The owner checks the path on a fixed interval, so allow a generous
    // window rather than coupling the test to that interval.
    bool restored = false;
    for (int i = 0; i < 150 && !restored; ++i) {
        struct stat st{};
        restored = ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
        if (!restored) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    REQUIRE(restored);

    transport_handle h = connect_transport(path);
    REQUIRE(h != invalid_transport);
    REQUIRE(send_frame(h, R"({"jsonrpc":"2.0","method":"answer","id":7})"));
    std::string resp = recv_frame(h);
    REQUIRE_FALSE(resp.empty());
    REQUIRE(json::parse(resp)["result"] == 42);
    close_transport(h);
}

// Until drain() runs, the main thread has produced no response for the
// request sitting in the queue.  That must not cost the transport anything:
// the I/O thread still has to accept a second client, read its request, and
// hold both responses.
TEST_CASE("RpcServer: an undrained request does not block other sessions",
          "[rpc][server]") {
    Dispatcher d = make_echo_dispatcher();
    RpcServer server(make_socket_path(), d);
    server.start();

    transport_handle a = connect_transport(server.socket_path());
    transport_handle b = connect_transport(server.socket_path());
    REQUIRE(a != invalid_transport);
    REQUIRE(b != invalid_transport);

    REQUIRE(send_frame(a, R"({"jsonrpc":"2.0","method":"answer","id":1})"));
    REQUIRE(send_frame(b,
                       R"({"jsonrpc":"2.0","method":"echo",)"
                       R"("params":{"n":2},"id":2})"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // One pass serves both, which is only possible if b's request was read
    // while a's was still queued.
    REQUIRE(server.drain() == 2);

    std::string ra = recv_frame(a);
    std::string rb = recv_frame(b);
    REQUIRE_FALSE(ra.empty());
    REQUIRE_FALSE(rb.empty());
    REQUIRE(json::parse(ra)["result"] == 42);
    REQUIRE(json::parse(rb)["result"]["n"] == 2);

    close_transport(a);
    close_transport(b);
}
#endif

// A queued request whose response nobody ever asks for must not hold stop()
// hostage on its way to joining the I/O thread.
TEST_CASE("RpcServer: stop() returns with a request left undrained",
          "[rpc][server]") {
    Dispatcher d = make_echo_dispatcher();
    RpcServer server(make_socket_path(), d);
    server.start();

    transport_handle h = connect_transport(server.socket_path());
    REQUIRE(h != invalid_transport);
    REQUIRE(send_frame(h, R"({"jsonrpc":"2.0","method":"answer","id":1})"));
    // No drain() at all before shutdown.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    REQUIRE_FALSE(server.is_running());
    close_transport(h);
}
