// POSIX implementation of socket_paths.h.
// Uses Unix Domain Sockets under /tmp.

#include "app/rpc/socket_paths.h"

#include "util/logger.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace idiff::rpc {

namespace {

constexpr const char* kTmpDir   = "/tmp";
constexpr const char* kPrefix   = "idiff-";
constexpr const char* kSuffix   = ".sock";

// Parse "idiff-<pid>.sock" -> pid; -1 if it does not match the pattern.
int parse_pid_from_filename(const std::string& fn) {
    const std::string prefix = kPrefix;
    const std::string suffix = kSuffix;
    if (fn.size() <= prefix.size() + suffix.size()) return -1;
    if (fn.compare(0, prefix.size(), prefix) != 0) return -1;
    if (fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) != 0)
        return -1;

    std::string mid = fn.substr(prefix.size(),
                                fn.size() - prefix.size() - suffix.size());
    if (mid.empty()) return -1;
    for (char c : mid) {
        if (c < '0' || c > '9') return -1;
    }
    try {
        return std::stoi(mid);
    } catch (...) {
        return -1;
    }
}

// Try to connect; return errno (0 on success).  We never block: a
// connect() to a Unix domain socket is non-blocking from the kernel's
// point of view if the listener is local and ready, so a vanilla
// blocking socket suffices for a probe.
int probe_connect(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return errno;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return ENAMETOOLONG;
    }
    std::memcpy(addr.sun_path, path.data(), path.size());

    int err = 0;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = errno;
    }
    ::close(fd);
    return err;
}

} // namespace

std::string compose_socket_path(int pid) {
    return std::string(kTmpDir) + "/" + kPrefix + std::to_string(pid) + kSuffix;
}

std::string compose_identity_label(int pid) {
    return std::string("idiff:") + std::to_string(pid);
}

std::vector<SocketProbe> sweep_stale_sockets() {
    std::vector<SocketProbe> out;

    DIR* dir = ::opendir(kTmpDir);
    if (!dir) {
        LOG_WARN("rpc-sweep: opendir(/tmp) failed: %s",
                 std::strerror(errno));
        return out;
    }

    while (dirent* ent = ::readdir(dir)) {
        std::string fn = ent->d_name;
        if (fn.compare(0, std::strlen(kPrefix), kPrefix) != 0) continue;
        if (fn.size() < std::strlen(kSuffix) ||
            fn.compare(fn.size() - std::strlen(kSuffix),
                       std::strlen(kSuffix), kSuffix) != 0) {
            continue;
        }

        SocketProbe pr;
        pr.path = std::string(kTmpDir) + "/" + fn;
        pr.pid  = parse_pid_from_filename(fn);

        // Only act on entries that are actually socket files.  A user
        // could create /tmp/idiff-<x>.sock as a regular file, and we
        // should not touch it.
        struct stat st{};
        if (::lstat(pr.path.c_str(), &st) != 0) continue;
        if (!S_ISSOCK(st.st_mode)) continue;

        int err = probe_connect(pr.path);
        if (err == 0) {
            pr.alive = true;
        } else if (err == ECONNREFUSED) {
            // Listener is gone but the inode is still here -- classic
            // stale-socket-after-crash.  Safe to unlink.
            if (::unlink(pr.path.c_str()) == 0) {
                pr.removed = true;
                LOG_INFO("rpc-sweep: removed stale socket %s",
                         pr.path.c_str());
            } else {
                LOG_WARN("rpc-sweep: unlink('%s') failed: %s",
                         pr.path.c_str(), std::strerror(errno));
            }
        } else {
            // EACCES, ETIMEDOUT, EAGAIN, etc. -- something we don't
            // own or that's transient.  Leave it alone.
            LOG_INFO("rpc-sweep: skipping %s (connect: %s)",
                     pr.path.c_str(), std::strerror(err));
        }
        out.push_back(std::move(pr));
    }
    ::closedir(dir);
    return out;
}

} // namespace idiff::rpc
