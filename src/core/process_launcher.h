#ifndef IDIFF_CORE_PROCESS_LAUNCHER_H
#define IDIFF_CORE_PROCESS_LAUNCHER_H

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace idiff {

// Outcome of a process started through ProcessLauncher::start().
struct ProcessResult {
    std::string name;         // the name passed to start()
    int exit_code = 0;        // 128 + signal number when killed by a signal
    std::string stderr_tail;  // last bytes the process wrote to stderr
};

// Starts external commands without blocking the caller and reports how
// they ended.  Children get /dev/null as stdin and stdout, and a pipe as
// stderr.  Not thread safe: start() and poll() must be called from one
// thread.  On Windows start() always fails.
class ProcessLauncher {
public:
    ProcessLauncher();
    ~ProcessLauncher();

    ProcessLauncher(const ProcessLauncher&) = delete;
    ProcessLauncher& operator=(const ProcessLauncher&) = delete;

    // Starts argv[0], looked up on PATH, with the remaining elements as
    // arguments.  No shell is involved.  `env_extra` entries replace
    // inherited variables of the same name.  Returns false and fills
    // `error` when the process cannot be started; no result is reported
    // for it later.
    bool start(const std::string& name,
               const std::vector<std::string>& argv,
               const std::vector<std::pair<std::string, std::string>>& env_extra,
               std::string& error);

    // Returns the processes that have ended since the previous call.
    std::vector<ProcessResult> poll();

    std::size_t running_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace idiff

#endif // IDIFF_CORE_PROCESS_LAUNCHER_H
