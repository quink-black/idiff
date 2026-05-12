#include "util/logger.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#  include <io.h>
#  define IDIFF_FILENO _fileno
#  define IDIFF_ISATTY _isatty
#else
#  include <unistd.h>
#  define IDIFF_FILENO ::fileno
#  define IDIFF_ISATTY ::isatty
#endif

namespace idiff::log {

namespace {

class NullSink : public ILogSink {
public:
    bool enabled(Level) const noexcept override { return false; }
    void write(const Record&) override {}
    void flush() override {}
};

NullSink& null_sink_instance() {
    static NullSink s;
    return s;
}

// Owned by the process via set_sink(); raw pointer published for fast
// access from sink().  When no custom sink is installed we point at
// null_sink_instance() so callers never have to null-check.
std::unique_ptr<ILogSink> g_owned_sink;
std::atomic<ILogSink*> g_active_sink{&null_sink_instance()};

// Strip the leading directory components so log lines stay readable.
// We only show the basename; the full path is rarely useful and bloats
// every line.
const char* short_basename(const char* path) noexcept {
    if (!path) return "";
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

void format_record(const Record& r, std::string& out) {
    using namespace std::chrono;
    auto sec  = time_point_cast<seconds>(r.ts);
    auto ms   = duration_cast<milliseconds>(r.ts - sec).count();
    std::time_t tt = system_clock::to_time_t(sec);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    char head[96];
    std::snprintf(head, sizeof(head),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03lld %-5s ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms),
                  level_name(r.level));
    out.append(head);

    // Compact thread id: hash to a small number to keep alignment.
    std::ostringstream oss;
    oss << r.tid;
    out.append("[");
    out.append(oss.str());
    out.append("] ");

    out.append(short_basename(r.file));
    out.push_back(':');
    char ln[16];
    std::snprintf(ln, sizeof(ln), "%d", r.line);
    out.append(ln);
    out.append(" ");

    out.append(r.message);
    out.push_back('\n');
}

} // namespace

const char* level_name(Level lvl) noexcept {
    switch (lvl) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
        case Level::Off:   return "OFF";
    }
    return "?";
}

std::string vformat(const char* fmt, std::va_list ap) {
    if (!fmt) return {};
    std::va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = std::vsnprintf(nullptr, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    std::vsnprintf(out.data(), out.size() + 1, fmt, ap);
    return out;
}

std::string format(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    return s;
}

ILogSink& sink() {
    return *g_active_sink.load(std::memory_order_acquire);
}

std::unique_ptr<ILogSink> set_sink(std::unique_ptr<ILogSink> s) {
    std::unique_ptr<ILogSink> previous = std::move(g_owned_sink);
    if (s) {
        g_active_sink.store(s.get(), std::memory_order_release);
        g_owned_sink = std::move(s);
    } else {
        g_active_sink.store(&null_sink_instance(), std::memory_order_release);
        g_owned_sink.reset();
    }
    return previous;
}

// ---- ConsoleSink ----------------------------------------------------------

struct ConsoleSink::Impl {
    std::mutex mtx;
    Level min_level = Level::Info;
    bool use_color = false;
};

ConsoleSink::ConsoleSink(Level min_level)
    : impl_(std::make_unique<Impl>()) {
    impl_->min_level = min_level;
    impl_->use_color = (IDIFF_ISATTY(IDIFF_FILENO(stderr)) != 0);
}

ConsoleSink::~ConsoleSink() = default;

bool ConsoleSink::enabled(Level lvl) const noexcept {
    return static_cast<int>(lvl) >= static_cast<int>(impl_->min_level);
}

void ConsoleSink::write(const Record& r) {
    std::string line;
    line.reserve(160);
    format_record(r, line);

    const char* color_pre = "";
    const char* color_post = "";
    if (impl_->use_color) {
        switch (r.level) {
            case Level::Trace: color_pre = "\033[37m"; break;
            case Level::Debug: color_pre = "\033[36m"; break;
            case Level::Info:  color_pre = ""; break;
            case Level::Warn:  color_pre = "\033[33m"; break;
            case Level::Error: color_pre = "\033[31m"; break;
            default: break;
        }
        if (color_pre[0]) color_post = "\033[0m";
    }

    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::fputs(color_pre, stderr);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputs(color_post, stderr);
}

void ConsoleSink::flush() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::fflush(stderr);
}

void ConsoleSink::set_min_level(Level lvl) noexcept {
    impl_->min_level = lvl;
}

// ---- RotatingFileSink -----------------------------------------------------
//
// Disk IO runs on a dedicated thread.  Producers append to a deque under
// a mutex and signal a condvar; the worker drains the queue, writes,
// and rotates when the file exceeds max_bytes.  Bounded queue length
// keeps memory bounded; on overflow we drop the oldest pending record
// and increment a drop counter that is emitted as a warning line on
// flush() / shutdown.

struct RotatingFileSink::Impl {
    std::string path;
    std::size_t max_bytes = 0;
    int max_files = 0;
    Level min_level = Level::Debug;

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool stop = false;
    std::size_t dropped = 0;
    std::thread worker;

    static constexpr std::size_t kQueueLimit = 4096;
};

namespace {

void rotate_files(const std::string& base, int max_files) {
    namespace fs = std::filesystem;
    if (max_files <= 1) {
        std::error_code ec;
        fs::remove(base, ec);
        return;
    }
    // Drop the oldest, then shift each backup up by one.
    std::error_code ec;
    fs::remove(base + "." + std::to_string(max_files - 1), ec);
    for (int i = max_files - 2; i >= 1; --i) {
        fs::rename(base + "." + std::to_string(i),
                   base + "." + std::to_string(i + 1), ec);
    }
    fs::rename(base, base + ".1", ec);
}

} // namespace

RotatingFileSink::RotatingFileSink(std::string path,
                                   std::size_t max_bytes,
                                   int max_files,
                                   Level min_level)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->max_bytes = max_bytes;
    impl_->max_files = max_files > 0 ? max_files : 1;
    impl_->min_level = min_level;

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(impl_->path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

    Impl* impl = impl_.get();
    impl_->worker = std::thread([impl]() {
        std::FILE* fp = std::fopen(impl->path.c_str(), "ab");
        std::size_t cur_size = 0;
        if (fp) {
            std::fseek(fp, 0, SEEK_END);
            long pos = std::ftell(fp);
            if (pos > 0) cur_size = static_cast<std::size_t>(pos);
        }

        std::deque<std::string> local;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(impl->mtx);
                impl->cv.wait(lk, [&] {
                    return impl->stop || !impl->queue.empty();
                });
                local.swap(impl->queue);
                if (impl->stop && local.empty()) break;
            }
            if (!fp) {
                fp = std::fopen(impl->path.c_str(), "ab");
                if (!fp) {
                    local.clear();
                    continue;
                }
                std::fseek(fp, 0, SEEK_END);
                long pos = std::ftell(fp);
                cur_size = pos > 0 ? static_cast<std::size_t>(pos) : 0;
            }
            while (!local.empty()) {
                const std::string& msg = local.front();
                std::fwrite(msg.data(), 1, msg.size(), fp);
                cur_size += msg.size();
                local.pop_front();
                if (impl->max_bytes > 0 && cur_size >= impl->max_bytes) {
                    std::fclose(fp);
                    fp = nullptr;
                    rotate_files(impl->path, impl->max_files);
                    fp = std::fopen(impl->path.c_str(), "ab");
                    cur_size = 0;
                    if (!fp) break;
                }
            }
            if (fp) std::fflush(fp);
        }
        if (fp) {
            // Emit a final marker if any records were dropped under
            // queue pressure so the operator can correlate gaps.
            std::size_t dropped = 0;
            {
                std::lock_guard<std::mutex> lk(impl->mtx);
                dropped = impl->dropped;
            }
            if (dropped > 0) {
                char tail[96];
                int n = std::snprintf(tail, sizeof(tail),
                                      "<logger dropped %zu records under queue pressure>\n",
                                      dropped);
                if (n > 0) std::fwrite(tail, 1, static_cast<std::size_t>(n), fp);
            }
            std::fflush(fp);
            std::fclose(fp);
        }
    });
}

RotatingFileSink::~RotatingFileSink() {
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool RotatingFileSink::enabled(Level lvl) const noexcept {
    return static_cast<int>(lvl) >= static_cast<int>(impl_->min_level);
}

void RotatingFileSink::write(const Record& r) {
    std::string line;
    line.reserve(160);
    format_record(r, line);
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (impl_->queue.size() >= Impl::kQueueLimit) {
            impl_->queue.pop_front();
            ++impl_->dropped;
        }
        impl_->queue.emplace_back(std::move(line));
    }
    impl_->cv.notify_one();
}

void RotatingFileSink::flush() {
    // The worker auto-flushes after each batch; we just wait for the
    // queue to drain.  For simplicity we busy-poll briefly; flush is
    // not on a hot path.
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            if (impl_->queue.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void RotatingFileSink::set_min_level(Level lvl) noexcept {
    impl_->min_level = lvl;
}

// ---- MultiSink ------------------------------------------------------------

MultiSink::MultiSink(std::vector<std::unique_ptr<ILogSink>> sinks)
    : sinks_(std::move(sinks)) {}

bool MultiSink::enabled(Level lvl) const noexcept {
    for (const auto& s : sinks_) {
        if (s && s->enabled(lvl)) return true;
    }
    return false;
}

void MultiSink::write(const Record& r) {
    for (auto& s : sinks_) {
        if (s && s->enabled(r.level)) s->write(r);
    }
}

void MultiSink::flush() {
    for (auto& s : sinks_) {
        if (s) s->flush();
    }
}

} // namespace idiff::log
