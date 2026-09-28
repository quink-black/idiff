// Unit tests for the stale-transport sweep.  POSIX only: a named pipe is a
// kernel object and never outlives its server, so the Windows sweep has no
// removal path to test.

#include "app/rpc/socket_paths.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <cstdio>

using idiff::rpc::SocketProbe;
using idiff::rpc::sweep_stale_sockets;

namespace {

const SocketProbe* find_probe(const std::vector<SocketProbe>& probes,
                              const std::string& path) {
    for (const auto& p : probes) {
        if (p.path == path) return &p;
    }
    return nullptr;
}

bool path_exists(const std::string& path) {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0;
}

// Leave a socket file with no listener behind: bind, then close without
// unlinking.  connect() on it answers ECONNREFUSED, like the leftover of a
// killed instance.
void make_orphan_socket_file(const std::string& path) {
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(fd);

    REQUIRE(path_exists(path));
}

std::string socket_path_for_pid(int pid) {
    return "/tmp/idiff-" + std::to_string(pid) + ".sock";
}

// A pid that has been waited for is dead, and stays dead for the length of a
// test run.
int reap_child() {
    pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::_exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    return static_cast<int>(child);
}

} // namespace

TEST_CASE("sweep leaves a socket file of a running pid alone",
          "[rpc][sweep]") {
    const std::string path = socket_path_for_pid(::getpid());
    make_orphan_socket_file(path);

    auto probes = sweep_stale_sockets();
    const SocketProbe* mine = find_probe(probes, path);
    REQUIRE(mine != nullptr);
    CHECK_FALSE(mine->alive);
    CHECK_FALSE(mine->removed);
    CHECK(path_exists(path));

    ::unlink(path.c_str());
}

TEST_CASE("sweep removes a socket file whose pid is gone",
          "[rpc][sweep]") {
    const std::string path = socket_path_for_pid(reap_child());
    make_orphan_socket_file(path);

    auto probes = sweep_stale_sockets();
    const SocketProbe* mine = find_probe(probes, path);
    REQUIRE(mine != nullptr);
    CHECK(mine->removed);
    CHECK_FALSE(path_exists(path));
}

TEST_CASE("sweep leaves a non-socket file alone", "[rpc][sweep]") {
    const std::string path = socket_path_for_pid(reap_child());
    ::unlink(path.c_str());
    FILE* fp = std::fopen(path.c_str(), "w");
    REQUIRE(fp != nullptr);
    std::fclose(fp);

    auto probes = sweep_stale_sockets();
    CHECK(find_probe(probes, path) == nullptr);
    CHECK(path_exists(path));

    ::unlink(path.c_str());
}

#endif // !_WIN32
