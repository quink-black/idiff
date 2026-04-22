#include "app/seedvr2_engine.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif

namespace idiff {

SeedVR2Engine::SeedVR2Engine(const std::filesystem::path& upscaler_path)
    : upscaler_path_(upscaler_path) {}

SeedVR2Engine::~SeedVR2Engine() {
    // Signal the worker to stop and wait for it to finish.
    // This is critical: the worker thread captures `this`, so we
    // must guarantee it has exited before any member is destroyed.
    cancel();
}

std::filesystem::path SeedVR2Engine::resolve_upscaler_path() {
    // 1. Check environment variable first
    const char* env_path = std::getenv("SEEDVR2_UPSCALER_PATH");
    if (env_path && env_path[0]) {
        return std::filesystem::path(env_path);
    }

    // 2. Check relative to executable directory
    std::filesystem::path exe_dir;
#ifdef _WIN32
    wchar_t exe_buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        exe_dir = std::filesystem::path(exe_buf).parent_path();
    }
#elif defined(__linux__)
    exe_dir = std::filesystem::read_symlink("/proc/self/exe").parent_path();
#elif defined(__APPLE__)
    uint32_t buf_size = 0;
    _NSGetExecutablePath(nullptr, &buf_size);
    std::vector<char> exe_buf(buf_size);
    if (_NSGetExecutablePath(exe_buf.data(), &buf_size) == 0) {
        exe_dir = std::filesystem::path(exe_buf.data()).parent_path();
    }
#endif

    if (!exe_dir.empty()) {
        auto candidate = exe_dir / "seedvr2-upscaler";
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
    }

    return {};  // Not found
}

std::string SeedVR2Engine::build_command(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    int scale, int tile_size, int tile_overlap,
    const std::string& model,
    const std::string& color_correction) const {
    // Path to the embedded Python interpreter
    auto python_exe = (upscaler_path_ / "python" / "python.exe").string();
    auto upscale_script = (upscaler_path_ / "app" / "upscale.py").string();
    auto model_dir = (upscaler_path_ / "models").string();

    // Wrap paths with spaces in quotes
    auto quote = [](const std::string& p) -> std::string {
        if (p.find(' ') != std::string::npos) {
            return "\"" + p + "\"";
        }
        return p;
    };

    std::ostringstream cmd;
    cmd << quote(python_exe) << " " << quote(upscale_script);
    cmd << " --input " << quote(input_path.string());
    cmd << " --output " << quote(output_path.string());
    cmd << " --scale " << scale;
    cmd << " --tile-size " << tile_size;
    cmd << " --tile-overlap " << tile_overlap;
    cmd << " --model " << quote(model);
    cmd << " --model-dir " << quote(model_dir);
    cmd << " --color-correction " << color_correction;
    cmd << " --debug";

    return cmd.str();
}

float SeedVR2Engine::parse_progress(const std::string& line) {
    // The seedvr2-upscaler outputs progress in several formats:
    //   "Progress: 42.5%"                          — explicit percentage
    //   "phase:upscale progress:0.425"              — fractional progress
    //   "Processing tile 2/4 [256x256]"             — tile-based progress
    // Try to match each pattern in turn.
    {
        auto pos = line.find("Progress:");
        if (pos != std::string::npos) {
            auto pct_pos = line.find('%', pos);
            if (pct_pos != std::string::npos) {
                std::string num = line.substr(pos + 9, pct_pos - pos - 9);
                try { return std::stof(num) / 100.0f; }
                catch (...) {}
            }
        }
    }
    {
        // Match "progress:0.xxx" pattern
        auto pos = line.find("progress:");
        if (pos != std::string::npos) {
            std::string rest = line.substr(pos + 9);
            size_t end = 0;
            while (end < rest.size() && (rest[end] == '.' ||
                   (rest[end] >= '0' && rest[end] <= '9'))) {
                ++end;
            }
            try { return std::stof(rest.substr(0, end)); }
            catch (...) {}
        }
    }
    {
        // Match "Processing tile N/M" pattern from debug output
        auto pos = line.find("Processing tile ");
        if (pos != std::string::npos) {
            auto rest = line.substr(pos + 16); // skip "Processing tile "
            auto slash = rest.find('/');
            if (slash != std::string::npos) {
                try {
                    int current = std::stoi(rest.substr(0, slash));
                    // Find end of total number
                    size_t end = slash + 1;
                    while (end < rest.size() && rest[end] >= '0' && rest[end] <= '9') ++end;
                    int total = std::stoi(rest.substr(slash + 1, end - slash - 1));
                    if (total > 0) {
                        return static_cast<float>(current) / static_cast<float>(total);
                    }
                } catch (...) {}
            }
        }
    }
    return -1.0f;  // No progress info in this line
}

bool SeedVR2Engine::start_inference(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    int scale, int tile_size, int tile_overlap,
    const std::string& model,
    const std::string& color_correction) {
    if (status_.load() == SREngineStatus::Running) {
        return false;  // Already running
    }

    if (!std::filesystem::exists(input_path)) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = {"io", "Input file does not exist: " + input_path.string()};
        status_.store(SREngineStatus::Failed);
        return false;
    }

    // Verify the upscaler directory structure
    auto python_exe = upscaler_path_ / "python" / "python.exe";
    auto upscale_script = upscaler_path_ / "app" / "upscale.py";
    if (!std::filesystem::exists(python_exe)) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = {"io", "Python interpreter not found: " + python_exe.string()};
        status_.store(SREngineStatus::Failed);
        return false;
    }
    if (!std::filesystem::exists(upscale_script)) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = {"io", "Upscale script not found: " + upscale_script.string()};
        status_.store(SREngineStatus::Failed);
        return false;
    }

    output_path_ = output_path;
    progress_.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = {};
        stderr_output_.clear();
    }
    status_.store(SREngineStatus::Running);
    cancel_requested_.store(false);

    // If a previous worker thread is still joinable (e.g. the engine
    // was reused after a completed/failed run), join it first.
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    std::string cmd_str = build_command(
        input_path, output_path, scale,
        tile_size, tile_overlap, model, color_correction);

    // Capture the upscaler root path for setting the working directory
    // and environment variables in the subprocess.
    std::filesystem::path upscaler_root = upscaler_path_;

    // Launch subprocess in a background worker thread (stored as a
    // member so we can join it in cancel() / destructor).
    worker_thread_ = std::thread([this, cmd_str, upscaler_root]() {
#ifdef _WIN32
        // Set environment variables required by the Python upscaler:
        //   PYTHONIOENCODING=utf-8  — prevent UnicodeEncodeError on Windows
        //                             when the debug logger prints emoji
        //   PYTHONPATH=<root>/app   — ensure Python can find the src/ package
        SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
        auto pythonpath = (upscaler_root / "app").wstring();
        SetEnvironmentVariableW(L"PYTHONPATH", pythonpath.c_str());

        // Create pipes for stdout and stderr
        HANDLE hStdOutRead, hStdOutWrite;
        HANDLE hStdErrRead, hStdErrWrite;
        SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

        CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0);
        CreatePipe(&hStdErrRead, &hStdErrWrite, &sa, 0);
        SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(hStdErrRead, HANDLE_FLAG_INHERIT, 0);

        PROCESS_INFORMATION pi = {};
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hStdOutWrite;
        si.hStdError = hStdErrWrite;

        // Convert command to wide string
        int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd_str.c_str(), -1, nullptr, 0);
        std::wstring wcmd(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cmd_str.c_str(), -1, &wcmd[0], wlen);

        // Set working directory to the upscaler root so relative paths
        // inside the Python script resolve correctly.
        auto cwd = upscaler_root.wstring();

        BOOL ok = CreateProcessW(
            nullptr, &wcmd[0], nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);

        CloseHandle(hStdOutWrite);
        CloseHandle(hStdErrWrite);

        if (!ok) {
            DWORD err = GetLastError();
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = {"subprocess", "CreateProcess failed with error code " + std::to_string(err)};
            status_.store(SREngineStatus::Failed);
            CloseHandle(hStdOutRead);
            CloseHandle(hStdErrRead);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(subprocess_mutex_);
            subprocess_handle_ = pi.hProcess;
        }

        // Read stdout and stderr concurrently to avoid deadlock.
        // If we read them sequentially, the unread pipe's buffer can
        // fill up and block the child process.
        std::string captured_stderr;
        std::thread stderr_reader([&captured_stderr, hStdErrRead, this]() {
            std::array<char, 4096> buf;
            DWORD bytesRead;
            while (ReadFile(hStdErrRead, buf.data(),
                            static_cast<DWORD>(buf.size()),
                            &bytesRead, nullptr) && bytesRead > 0) {
                std::string chunk(buf.data(), bytesRead);
                captured_stderr.append(chunk);
                float p = parse_progress(chunk);
                if (p >= 0) progress_.store(p);
            }
        });

        // Read stdout for progress information (main thread)
        {
            std::array<char, 4096> buf;
            DWORD bytesRead;
            while (ReadFile(hStdOutRead, buf.data(),
                            static_cast<DWORD>(buf.size()),
                            &bytesRead, nullptr) && bytesRead > 0) {
                std::string chunk(buf.data(), bytesRead);
                float p = parse_progress(chunk);
                if (p >= 0) progress_.store(p);
            }
        }
        stderr_reader.join();

        // Wait for process completion
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);

        CloseHandle(pi.hThread);
        CloseHandle(hStdOutRead);
        CloseHandle(hStdErrRead);

        // Close the process handle under the mutex so cancel() on
        // another thread never sees a stale handle.
        {
            std::lock_guard<std::mutex> lock(subprocess_mutex_);
            CloseHandle(pi.hProcess);
            subprocess_handle_ = nullptr;
        }

        // If cancel() was called while we were running, don't
        // overwrite the Idle status it set.
        if (cancel_requested_.load()) return;

        if (exit_code == 0) {
            progress_.store(1.0f);
            status_.store(SREngineStatus::Completed);
        } else {
            std::lock_guard<std::mutex> lock(error_mutex_);
            stderr_output_ = captured_stderr;
            // Build a descriptive error message including the last
            // few lines of stderr so the user can see what went wrong.
            std::string detail = "Upscaler exited with code " + std::to_string(exit_code);
            if (!captured_stderr.empty()) {
                // Extract the last meaningful lines from stderr
                // (skip trailing whitespace)
                auto end = captured_stderr.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) {
                    auto trimmed = captured_stderr.substr(0, end + 1);
                    // Find the last few lines
                    size_t lines_to_show = 0;
                    size_t pos = trimmed.size();
                    for (int i = 0; i < 10 && pos > 0; ++i) {
                        pos = trimmed.rfind('\n', pos - 1);
                        if (pos == std::string::npos) { pos = 0; break; }
                        ++lines_to_show;
                    }
                    detail += "\n\n--- stderr (last lines) ---\n";
                    detail += trimmed.substr(pos);
                }
            }
            last_error_ = {"subprocess", detail};
            status_.store(SREngineStatus::Failed);
        }
#else
        // POSIX implementation
        // Set environment variables for the child process
        setenv("PYTHONIOENCODING", "utf-8", 1);
        auto pythonpath = (upscaler_root / "app").string();
        setenv("PYTHONPATH", pythonpath.c_str(), 1);

        int pipe_out[2], pipe_err[2];
        pipe(pipe_out);
        pipe(pipe_err);

        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            close(pipe_out[0]);
            close(pipe_err[0]);
            dup2(pipe_out[1], STDOUT_FILENO);
            dup2(pipe_err[1], STDERR_FILENO);
            close(pipe_out[1]);
            close(pipe_err[1]);

            // Change to upscaler root directory
            chdir(upscaler_root.c_str());

            // Split command into args
            std::istringstream iss(cmd_str);
            std::vector<std::string> args;
            std::string arg;
            while (iss >> arg) args.push_back(arg);

            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);

            execvp(argv[0], argv.data());
            _exit(127);  // exec failed
        } else if (pid > 0) {
            // Parent
            close(pipe_out[1]);
            close(pipe_err[1]);
            {
                std::lock_guard<std::mutex> lock(subprocess_mutex_);
                subprocess_handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
            }

            // Read stdout and stderr concurrently to avoid deadlock.
            std::string captured_stderr;
            std::thread stderr_reader([&captured_stderr, &pipe_err, this]() {
                std::array<char, 4096> buf;
                ssize_t bytesRead;
                while ((bytesRead = read(pipe_err[0], buf.data(), buf.size())) > 0) {
                    std::string chunk(buf.data(), bytesRead);
                    captured_stderr.append(chunk);
                    float p = parse_progress(chunk);
                    if (p >= 0) progress_.store(p);
                }
            });

            // Read stdout for progress (main thread)
            {
                std::array<char, 4096> buf;
                ssize_t bytesRead;
                while ((bytesRead = read(pipe_out[0], buf.data(), buf.size())) > 0) {
                    std::string chunk(buf.data(), bytesRead);
                    float p = parse_progress(chunk);
                    if (p >= 0) progress_.store(p);
                }
            }
            stderr_reader.join();

            close(pipe_out[0]);
            close(pipe_err[0]);

            int wstatus;
            waitpid(pid, &wstatus, 0);

            {
                std::lock_guard<std::mutex> lock(subprocess_mutex_);
                subprocess_handle_ = nullptr;
            }

            // If cancel() was called, don't overwrite the Idle status.
            if (cancel_requested_.load()) return;

            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
                progress_.store(1.0f);
                status_.store(SREngineStatus::Completed);
            } else {
                int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
                std::lock_guard<std::mutex> lock(error_mutex_);
                stderr_output_ = captured_stderr;
                std::string detail = "Upscaler exited with code " + std::to_string(code);
                if (!captured_stderr.empty()) {
                    auto end = captured_stderr.find_last_not_of(" \t\r\n");
                    if (end != std::string::npos) {
                        auto trimmed = captured_stderr.substr(0, end + 1);
                        size_t pos = trimmed.size();
                        for (int i = 0; i < 10 && pos > 0; ++i) {
                            pos = trimmed.rfind('\n', pos - 1);
                            if (pos == std::string::npos) { pos = 0; break; }
                        }
                        detail += "\n\n--- stderr (last lines) ---\n";
                        detail += trimmed.substr(pos);
                    }
                }
                last_error_ = {"subprocess", detail};
                status_.store(SREngineStatus::Failed);
            }
        } else {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = {"subprocess", "Failed to fork subprocess"};
            status_.store(SREngineStatus::Failed);
        }
#endif
    });

    return true;
}

SREngineStatus SeedVR2Engine::get_status() const {
    return status_.load();
}

std::filesystem::path SeedVR2Engine::get_output_path() const {
    return output_path_;
}

bool SeedVR2Engine::cancel() {
    cancel_requested_.store(true);

    // Terminate the subprocess under the mutex so we never race with
    // the worker thread that also accesses subprocess_handle_.
    {
        std::lock_guard<std::mutex> lock(subprocess_mutex_);
#ifdef _WIN32
        if (subprocess_handle_) {
            TerminateProcess(subprocess_handle_, 1);
            // Do NOT CloseHandle here — the worker thread owns the
            // handle and will close it after WaitForSingleObject
            // returns (which happens immediately once we terminate
            // the process).
        }
#else
        if (subprocess_handle_) {
            kill(static_cast<pid_t>(
                reinterpret_cast<intptr_t>(subprocess_handle_)), SIGTERM);
        }
#endif
    }

    // Wait for the worker thread to finish.  After TerminateProcess
    // the pipes close and ReadFile returns, so the thread exits
    // promptly.  This guarantees no use-after-free on `this`.
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    if (status_.load() == SREngineStatus::Running) {
        status_.store(SREngineStatus::Idle);
    }
    return true;
}

SRError SeedVR2Engine::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

float SeedVR2Engine::get_progress() const {
    return progress_.load();
}

} // namespace idiff