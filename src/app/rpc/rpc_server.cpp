// Asio-backed transport for the JSON-RPC server.
//
// POSIX: Unix Domain Socket via asio::local::stream_protocol.
// Windows: Named Pipe via asio::windows::stream_handle.
//
// Concurrency model:
//
//   * POSIX: one io_context, one I/O thread.  All sockets, the
//     acceptor, and the framing buffers live on that thread.
//   * Windows: one io_context, one I/O thread, one accept thread.
//     The accept thread blocks on ConnectNamedPipe; on connect it
//     posts the HANDLE to the I/O thread which wraps it in
//     stream_handle and starts the session.
//
//   * Each session is shared_ptr-owned and kept alive by chained async
//     callbacks (typical Asio idiom).  A session ends when the peer
//     closes, the read fails, or stop() destroys the io_context.
//   * Per request, the I/O thread builds a PendingRequest holding the
//     parsed-but-not-dispatched JSON text and a std::promise<string>.
//     It pushes the PendingRequest onto a mutex-guarded queue and
//     blocks the session on the matching std::future inside an
//     async-callback continuation -- specifically, after queuing it
//     posts a follow-up handler that waits on the future, writes the
//     framed reply, and chains another async_read.
//
//     Only the response wait blocks; reads on OTHER sessions continue
//     to be processed on the same thread because we drive that wait
//     through io_context::run() too (the blocking happens on a worker
//     std::future::get(), but we keep the io_context responsive by
//     using a strand only per session, not globally).  In practice the
//     main thread drains the queue every GUI frame (~16 ms), so the
//     blocking wait is bounded.
//
//   * drain() runs on the main thread.  It pops every PendingRequest,
//     calls Dispatcher::handle_request on the GUI thread, and sets the
//     associated promise.  Because all App state mutation happens here,
//     no locks are needed inside handlers.

#include "app/rpc/rpc_server.h"

#include "app/rpc/rpc_dispatcher.h"
#include "util/logger.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace idiff::rpc {

namespace {

// Encode a 4-byte big-endian length prefix.
void write_be32(std::array<unsigned char, 4>& out, std::uint32_t value) {
    out[0] = static_cast<unsigned char>((value >> 24) & 0xFF);
    out[1] = static_cast<unsigned char>((value >> 16) & 0xFF);
    out[2] = static_cast<unsigned char>((value >>  8) & 0xFF);
    out[3] = static_cast<unsigned char>( value        & 0xFF);
}

std::uint32_t read_be32(const std::array<unsigned char, 4>& in) {
    return (static_cast<std::uint32_t>(in[0]) << 24)
         | (static_cast<std::uint32_t>(in[1]) << 16)
         | (static_cast<std::uint32_t>(in[2]) <<  8)
         |  static_cast<std::uint32_t>(in[3]);
}

#ifdef _WIN32

// Convert UTF-8 pipe path to wide string for CreateNamedPipeW.
std::wstring pipe_path_to_wide(const std::string& path) {
    if (path.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                  static_cast<int>(path.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                        static_cast<int>(path.size()), &out[0], len);
    return out;
}

#endif // _WIN32

} // namespace

// One pending request waiting for the main thread to dispatch it.  The
// response is delivered back through `result`; the I/O thread then
// writes the framed bytes on the originating socket.
struct PendingRequest {
    std::string request_json;
    std::promise<std::string> result;
};

struct RpcServer::Impl {
#ifdef _WIN32
    using session_socket = asio::windows::stream_handle;
#else
    using stream_protocol = asio::local::stream_protocol;
    using session_socket = stream_protocol::socket;
#endif

    Impl(std::string path, Dispatcher& d)
        : socket_path(std::move(path))
        , dispatcher(d)
        , io_context()
#ifdef _WIN32
        , accept_event(nullptr)
        , shutdown_event(nullptr)
#else
        , acceptor(io_context)
#endif
    {}

    // ---- Per-session state -------------------------------------------------
    //
    // `Session` is shared_ptr-managed; each async callback captures a
    // shared_from_this() to keep it alive across the wait.  When the
    // last callback releases the pointer, the session and its socket
    // are destroyed.
    struct Session : public std::enable_shared_from_this<Session> {
        Session(Impl& owner_in, session_socket socket_in)
            : owner(owner_in)
            , socket(std::move(socket_in)) {}

        Impl& owner;
        session_socket socket;
        std::array<unsigned char, 4> length_buf{};
        std::vector<unsigned char> payload_buf;

        void start() { read_length(); }

        void read_length() {
            auto self = shared_from_this();
            asio::async_read(
                socket, asio::buffer(length_buf),
                [self](const std::error_code& ec, std::size_t /*n*/) {
                    if (ec) {
                        if (ec != asio::error::eof
                                && ec != asio::error::operation_aborted
                                && ec != asio::error::connection_reset) {
                            LOG_WARN("rpc: read length failed: %s",
                                     ec.message().c_str());
                        }
                        return;
                    }
                    self->on_length();
                });
        }

        void on_length() {
            std::uint32_t len = read_be32(length_buf);
            if (len == 0) {
                read_length();
                return;
            }
            if (len > kMaxFrameBytes) {
                LOG_WARN("rpc: frame too large (%u bytes); closing", len);
                std::error_code ignore;
#ifdef _WIN32
                socket.close(ignore);
#else
                socket.shutdown(stream_protocol::socket::shutdown_both,
                                ignore);
#endif
                return;
            }
            payload_buf.assign(len, 0);
            auto self = shared_from_this();
            asio::async_read(
                socket, asio::buffer(payload_buf),
                [self](const std::error_code& ec, std::size_t /*n*/) {
                    if (ec) {
                        if (ec != asio::error::eof
                                && ec != asio::error::operation_aborted) {
                            LOG_WARN("rpc: read body failed: %s",
                                     ec.message().c_str());
                        }
                        return;
                    }
                    self->on_body();
                });
        }

        void on_body() {
            std::string request_json(
                reinterpret_cast<const char*>(payload_buf.data()),
                payload_buf.size());

            auto pending = std::make_shared<PendingRequest>();
            pending->request_json = std::move(request_json);
            std::future<std::string> fut = pending->result.get_future();

            {
                std::lock_guard<std::mutex> lk(owner.queue_mu);
                owner.queue.push_back(pending);
            }
            owner.queue_cv.notify_one();

            // Wait for the main thread to dispatch.
            std::string response = fut.get();

            if (response.empty()) {
                read_length();
                return;
            }

            if (response.size() > kMaxFrameBytes) {
                LOG_ERROR("rpc: response too large (%zu bytes); dropping",
                          response.size());
                return;
            }
            auto header = std::make_shared<std::array<unsigned char, 4>>();
            write_be32(*header, static_cast<std::uint32_t>(response.size()));
            auto body = std::make_shared<std::string>(std::move(response));

            auto self = shared_from_this();
            std::array<asio::const_buffer, 2> out = {
                asio::buffer(*header),
                asio::buffer(*body),
            };
            asio::async_write(
                socket, out,
                [self, header, body](const std::error_code& ec,
                                     std::size_t /*n*/) {
                    if (ec) {
                        if (ec != asio::error::operation_aborted) {
                            LOG_WARN("rpc: write failed: %s",
                                     ec.message().c_str());
                        }
                        return;
                    }
                    self->read_length();
                });
        }
    };

#ifdef _WIN32
    // ---- Windows accept loop -----------------------------------------------
    //
    // ConnectNamedPipe has no Asio async wrapper.  A dedicated accept
    // thread creates a pipe instance, issues ConnectNamedPipe with an
    // OVERLAPPED + manual-reset event, and waits via
    // WaitForMultipleObjects on (pipe-event, shutdown-event).  On
    // connect, the connected HANDLE is posted to the I/O thread which
    // wraps it in asio::windows::stream_handle and starts the session.
    void do_accept_win32(HANDLE first_pipe = INVALID_HANDLE_VALUE) {
        std::wstring wide_path = pipe_path_to_wide(socket_path);

        // The shutdown_event is the single source of truth for "stop
        // accepting".  Checking running.load() here would race with
        // start()/stop(): the accept thread is spawned async, and stop()
        // can flip running before this thread is even scheduled.  In
        // that case we still need to consume first_pipe (closing it on
        // the shutdown branch below) instead of leaking it.
        HANDLE hpipe = first_pipe;

        while (true) {
            // Create a fresh pipe instance unless one was passed in.
            // Bail out before allocation if shutdown was already signaled.
            if (hpipe == INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) {
                    break;
                }
                hpipe = CreateNamedPipeW(
                    wide_path.c_str(),
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
                    PIPE_UNLIMITED_INSTANCES,
                    8192,  // outbound buffer
                    8192,  // inbound buffer
                    0,     // default timeout
                    nullptr);
                if (hpipe == INVALID_HANDLE_VALUE) {
                    LOG_WARN("rpc: CreateNamedPipeW failed: %lu",
                             GetLastError());
                    Sleep(100);
                    continue;
                }
            }

            OVERLAPPED olap{};
            olap.hEvent = accept_event;
            BOOL ok = ConnectNamedPipe(hpipe, &olap);
            DWORD last_err = GetLastError();

            // ERROR_PIPE_CONNECTED means a client connected between
            // CreateNamedPipeW and ConnectNamedPipe -- skip the wait.
            bool already_connected = (!ok && last_err == ERROR_PIPE_CONNECTED);

            if (!ok && !already_connected && last_err != ERROR_IO_PENDING) {
                LOG_WARN("rpc: ConnectNamedPipe failed: %lu", last_err);
                CloseHandle(hpipe);
                hpipe = INVALID_HANDLE_VALUE;
                Sleep(100);
                continue;
            }

            if (!already_connected) {
                // Wait for either a client connection or shutdown signal.
                HANDLE wait_handles[2] = { accept_event, shutdown_event };
                DWORD wait_result = WaitForMultipleObjects(
                    2, wait_handles, FALSE, INFINITE);

                if (wait_result == WAIT_OBJECT_0 + 1) {
                    // Shutdown signaled -- clean up and exit.
                    DisconnectNamedPipe(hpipe);
                    CloseHandle(hpipe);
                    break;
                }

                if (wait_result != WAIT_OBJECT_0) {
                    // Unexpected wait result (e.g. abandoned mutex).
                    DisconnectNamedPipe(hpipe);
                    CloseHandle(hpipe);
                    hpipe = INVALID_HANDLE_VALUE;
                    continue;
                }

                // Client connected via overlapped completion.
                // Reset the event for the next ConnectNamedPipe call.
                ResetEvent(accept_event);
            }

            // Post the connected HANDLE to the I/O thread.  The I/O
            // thread wraps it in stream_handle (which calls
            // CreateIoCompletionPort) and starts the session.
            asio::post(io_context, [this, hpipe]() {
                asio::error_code ec;
                auto sock = session_socket(io_context);
                sock.assign(hpipe, ec);
                if (ec) {
                    LOG_ERROR("rpc: stream_handle assign failed: %s",
                              ec.message().c_str());
                    CloseHandle(hpipe);
                    return;
                }
                auto session =
                    std::make_shared<Session>(*this, std::move(sock));
                session->start();
            });

            // Next iteration creates a fresh pipe instance.
            hpipe = INVALID_HANDLE_VALUE;
        }
    }
#else
    // ---- POSIX accept loop -------------------------------------------------

    void do_accept() {
        acceptor.async_accept(
            [this](const std::error_code& ec,
                   stream_protocol::socket sock) {
                if (ec) {
                    if (ec != asio::error::operation_aborted) {
                        LOG_WARN("rpc: accept failed: %s",
                                 ec.message().c_str());
                    }
                    return;
                }
                auto session =
                    std::make_shared<Session>(*this, std::move(sock));
                session->start();
                do_accept();
            });
    }
#endif

    // ---- Members -----------------------------------------------------------

    std::string socket_path;
    Dispatcher& dispatcher;

    asio::io_context io_context;
    // Prevents io_context::run() from exiting when there are no pending
    // async operations.  Without this, on Windows the I/O thread exits
    // before the accept thread posts the first session handle.
    using work_guard_type = asio::executor_work_guard<asio::io_context::executor_type>;
    std::unique_ptr<work_guard_type> work_guard;

#ifdef _WIN32
    HANDLE accept_event;     // manual-reset event for ConnectNamedPipe
    HANDLE shutdown_event;   // manual-reset event for stop() signal
    std::thread accept_thread;
#else
    stream_protocol::acceptor acceptor;
#endif

    std::thread io_thread;
    std::atomic<bool> running{false};

    std::mutex queue_mu;
    std::condition_variable queue_cv;  // reserved for future blocking-drain support
    std::deque<std::shared_ptr<PendingRequest>> queue;
};

RpcServer::RpcServer(std::string socket_path, Dispatcher& dispatcher)
    : impl_(std::make_unique<Impl>(std::move(socket_path), dispatcher)) {}

RpcServer::~RpcServer() {
    stop();
}

void RpcServer::start() {
    if (impl_->running.load()) {
        return;
    }

#ifdef _WIN32
    // Create manual-reset events for the accept loop.
    impl_->accept_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->accept_event) {
        throw std::system_error(
            std::error_code(static_cast<int>(GetLastError()),
                            std::system_category()),
            "rpc: CreateEvent (accept) failed");
    }
    impl_->shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->shutdown_event) {
        CloseHandle(impl_->accept_event);
        impl_->accept_event = nullptr;
        throw std::system_error(
            std::error_code(static_cast<int>(GetLastError()),
                            std::system_category()),
            "rpc: CreateEvent (shutdown) failed");
    }

    // Create the first pipe instance before starting threads so that
    // callers can connect immediately after start() returns.
    std::wstring wide_path = pipe_path_to_wide(impl_->socket_path);
    HANDLE first_pipe = CreateNamedPipeW(
        wide_path.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
        PIPE_UNLIMITED_INSTANCES,
        8192, 8192, 0, nullptr);
    if (first_pipe == INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->accept_event);
        CloseHandle(impl_->shutdown_event);
        impl_->accept_event = nullptr;
        impl_->shutdown_event = nullptr;
        throw std::system_error(
            std::error_code(static_cast<int>(GetLastError()),
                            std::system_category()),
            "rpc: CreateNamedPipeW failed");
    }

    impl_->running.store(true);

    // Restart io_context (required after stop() which calls
    // io_context.stop()) and recreate the work guard.
    impl_->io_context.restart();
    impl_->work_guard = std::make_unique<decltype(impl_->work_guard)::element_type>(
        asio::make_work_guard(impl_->io_context));

    // Start the I/O thread first so it can receive posted handles.
    impl_->io_thread = std::thread([this] {
        try {
            impl_->io_context.run();
        } catch (const std::exception& ex) {
            LOG_ERROR("rpc: io_context.run threw: %s", ex.what());
        }
    });

    // Start the accept thread -- pass the pre-created first pipe.
    impl_->accept_thread = std::thread([this, first_pipe] {
        try {
            impl_->do_accept_win32(first_pipe);
        } catch (const std::exception& ex) {
            LOG_ERROR("rpc: accept thread threw: %s", ex.what());
        }
    });
#else
    // Unlink any stale socket file from a previous run.
    if (::unlink(impl_->socket_path.c_str()) != 0 && errno != ENOENT) {
        LOG_WARN("rpc: unlink('%s') failed: %s",
                 impl_->socket_path.c_str(), std::strerror(errno));
    }

    using stream_protocol = asio::local::stream_protocol;
    stream_protocol::endpoint ep(impl_->socket_path);

    std::error_code ec;
    impl_->acceptor.open(ep.protocol(), ec);
    if (ec) {
        throw std::system_error(ec, "rpc: acceptor.open");
    }
    impl_->acceptor.bind(ep, ec);
    if (ec) {
        throw std::system_error(ec,
            "rpc: bind(" + impl_->socket_path + ")");
    }
    impl_->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::system_error(ec, "rpc: listen");
    }

    impl_->do_accept();
    impl_->running.store(true);

    impl_->io_context.restart();
    impl_->work_guard = std::make_unique<decltype(impl_->work_guard)::element_type>(
        asio::make_work_guard(impl_->io_context));

    impl_->io_thread = std::thread([this] {
        try {
            impl_->io_context.run();
        } catch (const std::exception& ex) {
            LOG_ERROR("rpc: io_context.run threw: %s", ex.what());
        }
    });
#endif

    LOG_INFO("rpc: server listening on %s", impl_->socket_path.c_str());
}

void RpcServer::stop() {
    if (!impl_->running.load()) {
        return;
    }
    impl_->running.store(false);

#ifdef _WIN32
    // Signal the accept thread to exit, then join it.
    if (impl_->shutdown_event) {
        SetEvent(impl_->shutdown_event);
    }
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }

    // Release the work guard so io_context::run() can exit naturally.
    impl_->work_guard.reset();
    impl_->io_context.stop();

    if (impl_->io_thread.joinable()) {
        impl_->io_thread.join();
    }

    if (impl_->accept_event) {
        CloseHandle(impl_->accept_event);
        impl_->accept_event = nullptr;
    }
    if (impl_->shutdown_event) {
        CloseHandle(impl_->shutdown_event);
        impl_->shutdown_event = nullptr;
    }
#else
    // Cancel pending accepts / reads, then join.
    std::error_code ignore;
    impl_->acceptor.close(ignore);
    impl_->io_context.stop();

    if (impl_->io_thread.joinable()) {
        impl_->io_thread.join();
    }

    if (::unlink(impl_->socket_path.c_str()) != 0 && errno != ENOENT) {
        LOG_WARN("rpc: unlink('%s') on shutdown failed: %s",
                 impl_->socket_path.c_str(), std::strerror(errno));
    }
#endif

    // Drain any still-queued requests by completing their promises with
    // an empty response so blocked I/O-thread callbacks don't deadlock.
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mu);
        for (auto& p : impl_->queue) {
            try {
                p->result.set_value(std::string{});
            } catch (...) {
                // Promise might already be satisfied.
            }
        }
        impl_->queue.clear();
    }

    // Reset io_context so a future start() can reuse this object.
    impl_->io_context.restart();
}

std::size_t RpcServer::drain() {
    std::deque<std::shared_ptr<PendingRequest>> batch;
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mu);
        batch.swap(impl_->queue);
    }
    for (auto& pending : batch) {
        std::string response =
            impl_->dispatcher.handle_request(pending->request_json);
        try {
            pending->result.set_value(std::move(response));
        } catch (const std::future_error& ex) {
            LOG_WARN("rpc: promise.set_value failed: %s", ex.what());
        }
    }
    return batch.size();
}

const std::string& RpcServer::socket_path() const noexcept {
    return impl_->socket_path;
}

bool RpcServer::is_running() const noexcept {
    return impl_->running.load();
}

} // namespace idiff::rpc
