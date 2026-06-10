// Windows implementation of socket_paths.h.
// Named pipes replace Unix Domain Sockets: \\.\pipe\idiff-<pid>

#include "app/rpc/socket_paths.h"

#include "util/logger.h"

#include <cstdint>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace idiff::rpc {

namespace {

constexpr const char* kPipePrefix = "\\\\.\\pipe\\idiff-";

// Parse "idiff-<pid>" -> pid; -1 if it does not match the pattern.
int parse_pid_from_name(const std::string& fn) {
    const std::string prefix = "idiff-";
    if (fn.size() <= prefix.size()) return -1;
    if (fn.compare(0, prefix.size(), prefix) != 0) return -1;

    std::string mid = fn.substr(prefix.size());
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

// Wide-string conversion for FindFirstFileW pattern.
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                        static_cast<int>(s.size()), &out[0], len);
    return out;
}

std::string to_utf8(const wchar_t* ws, int len) {
    if (len <= 0) return {};
    int blen = WideCharToMultiByte(CP_UTF8, 0, ws, len,
                                   nullptr, 0, nullptr, nullptr);
    if (blen <= 0) return {};
    std::string out(blen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, len,
                        &out[0], blen, nullptr, nullptr);
    return out;
}

} // namespace

std::string compose_socket_path(int pid) {
    return std::string(kPipePrefix) + std::to_string(pid);
}

std::string compose_identity_label(int pid) {
    return std::string("idiff:") + std::to_string(pid);
}

std::vector<SocketProbe> sweep_stale_sockets() {
    std::vector<SocketProbe> out;

    // FindFirstFileW on \\.\pipe\idiff-* enumerates named pipes that
    // exist right now.  A pipe is listed only while the owning server
    // holds the HANDLE, so enumeration alone is sufficient for liveness
    // -- no separate probe or unlink step is needed.
    const std::string pattern = std::string(kPipePrefix) + "*";
    std::wstring wpattern = to_wide(pattern);
    if (wpattern.empty()) {
        LOG_WARN("rpc-sweep: failed to convert pattern to wide string");
        return out;
    }

    WIN32_FIND_DATAW ffd;
    HANDLE hfind = FindFirstFileW(wpattern.c_str(), &ffd);
    if (hfind == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) {
            LOG_WARN("rpc-sweep: FindFirstFileW failed: %lu", err);
        }
        return out;
    }

    do {
        std::string name = to_utf8(ffd.cFileName,
                                   static_cast<int>(wcslen(ffd.cFileName)));
        if (name.empty()) continue;

        SocketProbe pr;
        pr.path = std::string(kPipePrefix) + name;
        pr.pid = parse_pid_from_name(name);
        pr.alive = true;   // enumerated => server HANDLE is live
        pr.removed = false; // named pipes auto-cleanup; no unlink
        out.push_back(std::move(pr));
    } while (FindNextFileW(hfind, &ffd));

    FindClose(hfind);
    return out;
}

} // namespace idiff::rpc
