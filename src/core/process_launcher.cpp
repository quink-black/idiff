#include "core/process_launcher.h"

#ifndef _WIN32

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace idiff {

namespace {

constexpr std::size_t kStderrTailBytes = 512;

void set_cloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace

struct ProcessLauncher::Impl {
    struct Child {
        pid_t pid = -1;
        int stderr_fd = -1;
        std::string name;
        std::string tail;
    };
    std::vector<Child> children;

    static void drain(Child& c) {
        char buf[1024];
        for (;;) {
            const ssize_t n = ::read(c.stderr_fd, buf, sizeof(buf));
            if (n <= 0) return;
            c.tail.append(buf, static_cast<std::size_t>(n));
            if (c.tail.size() > kStderrTailBytes) {
                c.tail.erase(0, c.tail.size() - kStderrTailBytes);
            }
        }
    }
};

ProcessLauncher::ProcessLauncher() : impl_(std::make_unique<Impl>()) {}

ProcessLauncher::~ProcessLauncher() {
    for (auto& c : impl_->children) ::close(c.stderr_fd);
}

bool ProcessLauncher::start(
        const std::string& name,
        const std::vector<std::string>& argv,
        const std::vector<std::pair<std::string, std::string>>& env_extra,
        std::string& error) {
    if (argv.empty() || argv[0].empty()) {
        error = "empty command";
        return false;
    }

    int fds[2];
    if (::pipe(fds) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    set_cloexec(fds[0]);
    set_cloexec(fds[1]);
    set_nonblocking(fds[0]);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);

    std::vector<std::string> env_storage;
    for (char** e = environ; e && *e; ++e) {
        const char* eq = std::strchr(*e, '=');
        const std::size_t len = eq ? static_cast<std::size_t>(eq - *e)
                                   : std::strlen(*e);
        bool overridden = false;
        for (const auto& kv : env_extra) {
            if (kv.first.size() == len &&
                std::strncmp(kv.first.c_str(), *e, len) == 0) {
                overridden = true;
                break;
            }
        }
        if (!overridden) env_storage.emplace_back(*e);
    }
    for (const auto& kv : env_extra) {
        env_storage.push_back(kv.first + "=" + kv.second);
    }
    std::vector<char*> envp;
    for (auto& s : env_storage) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<char*> argp;
    std::vector<std::string> argv_copy = argv;
    for (auto& s : argv_copy) argp.push_back(s.data());
    argp.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, argv_copy[0].c_str(), &actions,
                                  nullptr, argp.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        error = argv[0] + ": " + std::strerror(rc);
        return false;
    }

    Impl::Child child;
    child.pid = pid;
    child.stderr_fd = fds[0];
    child.name = name;
    impl_->children.push_back(std::move(child));
    return true;
}

std::vector<ProcessResult> ProcessLauncher::poll() {
    std::vector<ProcessResult> finished;
    auto& children = impl_->children;
    for (std::size_t i = 0; i < children.size();) {
        auto& c = children[i];
        Impl::drain(c);
        int status = 0;
        const pid_t r = ::waitpid(c.pid, &status, WNOHANG);
        if (r == 0) {
            ++i;
            continue;
        }
        Impl::drain(c);
        ProcessResult result;
        result.name = c.name;
        if (r < 0) {
            result.exit_code = -1;
        } else if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
        }
        result.stderr_tail = std::move(c.tail);
        ::close(c.stderr_fd);
        finished.push_back(std::move(result));
        children.erase(children.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return finished;
}

std::size_t ProcessLauncher::running_count() const noexcept {
    return impl_->children.size();
}

} // namespace idiff

#else // _WIN32

namespace idiff {

struct ProcessLauncher::Impl {};

ProcessLauncher::ProcessLauncher() : impl_(std::make_unique<Impl>()) {}
ProcessLauncher::~ProcessLauncher() = default;

bool ProcessLauncher::start(
        const std::string&, const std::vector<std::string>&,
        const std::vector<std::pair<std::string, std::string>>&,
        std::string& error) {
    error = "external actions are not supported on this platform";
    return false;
}

std::vector<ProcessResult> ProcessLauncher::poll() { return {}; }

std::size_t ProcessLauncher::running_count() const noexcept { return 0; }

} // namespace idiff

#endif
