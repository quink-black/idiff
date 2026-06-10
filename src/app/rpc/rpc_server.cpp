// Asio-backed UDS transport for the JSON-RPC server.
//
// Concurrency model:
//
//   * One io_context, one I/O thread.  All sockets, the acceptor, and
//     the framing buffers live on that thread.
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
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
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

#include <unistd.h>

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

} // namespace

// One pending request waiting for the main thread to dispatch it.  The
// response is delivered back through `result`; the I/O thread then
// writes the framed bytes on the originating socket.
struct PendingRequest {
    std::string request_json;
    std::promise<std::string> result;
};

struct RpcServer::Impl {
    using stream_protocol = asio::local::stream_protocol;

    Impl(std::string path, Dispatcher& d)
        : socket_path(std::move(path))
        , dispatcher(d)
        , io_context()
        , acceptor(io_context) {}

    // ---- Per-session state -------------------------------------------------
    //
    // `Session` is shared_ptr-managed; each async callback captures a
    // shared_from_this() to keep it alive across the wait.  When the
    // last callback releases the pointer, the session and its socket
    // are destroyed.
    struct Session : public std::enable_shared_from_this<Session> {
        Session(Impl& owner_in, stream_protocol::socket socket_in)
            : owner(owner_in)
            , socket(std::move(socket_in)) {}

        Impl& owner;
        stream_protocol::socket socket;
        std::array<unsigned char, 4> length_buf{};
        std::vector<unsigned char> payload_buf;

        void start() { read_length(); }

        void read_length() {
            auto self = shared_from_this();
            asio::async_read(
                socket, asio::buffer(length_buf),
                [self](const std::error_code& ec, std::size_t /*n*/) {
                    if (ec) {
                        // EOF or closed -- drop session silently unless
                        // it's an unusual error worth logging.
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
                // Empty frame -- treat as a noop and read the next.
                read_length();
                return;
            }
            if (len > kMaxFrameBytes) {
                LOG_WARN("rpc: frame too large (%u bytes); closing", len);
                std::error_code ignore;
                socket.shutdown(stream_protocol::socket::shutdown_both,
                                ignore);
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

            // Hand off to the main thread.  We push (request, promise)
            // into the shared queue, then await the future on this I/O
            // thread.  Multiple concurrent sessions each wait on their
            // own future, so a slow request only blocks its own session
            // -- other sessions continue to be served by the same
            // io_context as long as the main thread keeps draining.
            auto pending = std::make_shared<PendingRequest>();
            pending->request_json = std::move(request_json);
            std::future<std::string> fut = pending->result.get_future();

            {
                std::lock_guard<std::mutex> lk(owner.queue_mu);
                owner.queue.push_back(pending);
            }
            owner.queue_cv.notify_one();

            // Wait for the main thread to dispatch.  This blocks the
            // I/O thread for this session, but does NOT block other
            // sessions if they had already been dispatched -- their
            // writes are independent.  In practice the main thread
            // drains every frame, so the wait is bounded.
            std::string response = fut.get();

            if (response.empty()) {
                // Notification -- no reply, just keep reading.
                read_length();
                return;
            }

            // Frame and write the response.
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

    // ---- Members -----------------------------------------------------------

    std::string socket_path;
    Dispatcher& dispatcher;

    asio::io_context io_context;
    stream_protocol::acceptor acceptor;
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

    // Unlink any stale socket file from a previous run.  We don't care
    // if the unlink fails because the path didn't exist (ENOENT); other
    // errors are reported but do not prevent the bind attempt -- bind()
    // itself will surface the conflict more usefully.
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
    impl_->io_thread = std::thread([this] {
        try {
            impl_->io_context.run();
        } catch (const std::exception& ex) {
            LOG_ERROR("rpc: io_context.run threw: %s", ex.what());
        }
    });

    LOG_INFO("rpc: server listening on %s", impl_->socket_path.c_str());
}

void RpcServer::stop() {
    if (!impl_->running.load()) {
        return;
    }
    impl_->running.store(false);

    // Cancel pending accepts / reads, then join.
    std::error_code ignore;
    impl_->acceptor.close(ignore);
    impl_->io_context.stop();

    if (impl_->io_thread.joinable()) {
        impl_->io_thread.join();
    }

    // Drain any still-queued requests by completing their promises with
    // an InternalError so blocked I/O-thread callbacks (if any survived
    // io_context.stop) don't deadlock.  In practice io_context.stop()
    // already abandoned them, but doing this is cheap insurance.
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

    if (::unlink(impl_->socket_path.c_str()) != 0 && errno != ENOENT) {
        LOG_WARN("rpc: unlink('%s') on shutdown failed: %s",
                 impl_->socket_path.c_str(), std::strerror(errno));
    }

    // Reset io_context so a future start() can reuse this object.  Asio
    // requires restart() before run() can be called again on the same
    // context.
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
