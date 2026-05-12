#ifndef IDIFF_UTIL_LOGGER_H
#define IDIFF_UTIL_LOGGER_H

// Lightweight structured logger.
//
// Design contract:
//   * Process-wide accessor `sink()` always returns a valid sink; before
//     `set_sink()` is called it returns a no-op sink, so logging from
//     static initializers and tests is safe.
//   * Sinks are responsible for thread safety and level filtering.
//   * The logging macros short-circuit on `enabled(level)` so disabled
//     trace/debug calls do not pay for argument formatting.
//
// To keep the dependency footprint minimal the formatter is a tiny
// printf-style wrapper around std::snprintf.  This is enough for every
// call site in idiff today.  Switching to spdlog later only requires
// providing a SpdlogSink (no call-site changes).

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace idiff::log {

enum class Level {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,
};

const char* level_name(Level lvl) noexcept;

struct Record {
    Level level = Level::Info;
    const char* file = "";
    int line = 0;
    const char* func = "";
    std::string message;
    std::thread::id tid = std::this_thread::get_id();
    std::chrono::system_clock::time_point ts = std::chrono::system_clock::now();
};

class ILogSink {
public:
    virtual ~ILogSink() = default;

    // Called on every log macro invocation before the message is
    // formatted.  Returning false short-circuits the call.
    virtual bool enabled(Level lvl) const noexcept = 0;

    // Emit a fully-formed record.  The sink owns thread safety.
    virtual void write(const Record& r) = 0;

    // Force any buffered records to durable storage.
    virtual void flush() = 0;
};

// Returns the active sink.  Always non-null.  Until set_sink() is called
// the returned sink is a no-op (enabled() == false for every level).
ILogSink& sink();

// Install a new sink and return the previous one.  Passing nullptr
// installs the no-op sink.  Safe to call multiple times; not safe to
// call concurrently with logging.
std::unique_ptr<ILogSink> set_sink(std::unique_ptr<ILogSink> s);

// Convenience formatter used by the logging macros.  Exposed so test
// helpers can build Records directly without going through a macro.
std::string format(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

std::string vformat(const char* fmt, std::va_list ap);

// ---- Sink implementations -------------------------------------------------

// Writes one line per record to stderr, colorized when the stream is a
// TTY.  Thread-safe via internal mutex.
class ConsoleSink : public ILogSink {
public:
    explicit ConsoleSink(Level min_level = Level::Info);
    ~ConsoleSink() override;
    bool enabled(Level lvl) const noexcept override;
    void write(const Record& r) override;
    void flush() override;
    void set_min_level(Level lvl) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Appends one line per record to a file, rotating once `max_bytes` is
// reached.  Up to `max_files` historical files are kept (logger.log,
// logger.log.1, ..., logger.log.<max_files-1>).  Disk IO happens on a
// background thread so the main loop never blocks.
class RotatingFileSink : public ILogSink {
public:
    RotatingFileSink(std::string path,
                     std::size_t max_bytes,
                     int max_files,
                     Level min_level = Level::Debug);
    ~RotatingFileSink() override;

    bool enabled(Level lvl) const noexcept override;
    void write(const Record& r) override;
    void flush() override;
    void set_min_level(Level lvl) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Multiplexes records to several sinks.  Any sink that returns true
// from enabled() participates; flush() forwards to all sinks.
class MultiSink : public ILogSink {
public:
    explicit MultiSink(std::vector<std::unique_ptr<ILogSink>> sinks);
    bool enabled(Level lvl) const noexcept override;
    void write(const Record& r) override;
    void flush() override;

private:
    std::vector<std::unique_ptr<ILogSink>> sinks_;
};

} // namespace idiff::log

// ---- Macros ---------------------------------------------------------------

#define IDIFF_LOG_AT(LVL, ...)                                                 \
    do {                                                                       \
        auto& _idiff_sink = ::idiff::log::sink();                              \
        if (_idiff_sink.enabled(LVL)) {                                        \
            ::idiff::log::Record _idiff_rec;                                   \
            _idiff_rec.level   = (LVL);                                        \
            _idiff_rec.file    = __FILE__;                                     \
            _idiff_rec.line    = __LINE__;                                     \
            _idiff_rec.func    = __func__;                                     \
            _idiff_rec.message = ::idiff::log::format(__VA_ARGS__);            \
            _idiff_sink.write(_idiff_rec);                                     \
        }                                                                      \
    } while (0)

#define LOG_TRACE(...) IDIFF_LOG_AT(::idiff::log::Level::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) IDIFF_LOG_AT(::idiff::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...)  IDIFF_LOG_AT(::idiff::log::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)  IDIFF_LOG_AT(::idiff::log::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) IDIFF_LOG_AT(::idiff::log::Level::Error, __VA_ARGS__)

#endif // IDIFF_UTIL_LOGGER_H
