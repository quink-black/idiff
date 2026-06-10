// Helpers for reasoning about the RPC transport path:
//
//   * compose_socket_path(pid)       -- platform-specific path
//                                       (POSIX: /tmp/idiff-<pid>.sock,
//                                        Windows: \\.\pipe\idiff-<pid>)
//   * compose_identity_label(pid)    -- "idiff:<pid>"  (UI chip + window title)
//   * sweep_stale_sockets()          -- enumerates existing transport paths,
//                                       probes each for liveness.  On POSIX,
//                                       removes stale socket files left by
//                                       crashed instances.  On Windows, named
//                                       pipes auto-cleanup so no removal is
//                                       needed.
//
// Why this lives in the rpc/ subtree rather than App: the same logic is
// useful to any future caller (e.g. an idiff CLI helper) and has no
// dependency on App / SDL / OpenCV.

#ifndef IDIFF_RPC_SOCKET_PATHS_H
#define IDIFF_RPC_SOCKET_PATHS_H

#include <cstddef>
#include <string>
#include <vector>

namespace idiff::rpc {

// Platform-specific transport path (POSIX: /tmp/idiff-<pid>.sock,
// Windows: \\.\pipe\idiff-<pid>).
std::string compose_socket_path(int pid);

// "idiff:<pid>" -- the user-facing identity tag mirrored in the window
// title and status-bar chip so the user can correlate a GUI window with
// a given /tmp/idiff-*.sock.
std::string compose_identity_label(int pid);

// One result of sweep_stale_sockets().
struct SocketProbe {
    std::string path;        // full transport path (socket or pipe)
    int pid = -1;            // parsed from the filename, -1 if unparseable
    bool alive = false;      // a listener owns it (POSIX: connect() ok; Windows: enumerated)
    bool removed = false;    // we unlinked it during sweep (POSIX-only; always false on Windows)
};

// Find every idiff transport path, classify each (alive vs stale), and
// remove the stale ones on POSIX.  "Stale" means the path exists but no
// process is listening; this happens after a hard kill / crash where
// the socket file outlived the idiff process.  On Windows, named pipes
// are kernel objects that vanish when the server exits, so enumeration
// alone is sufficient and `removed` is always false.
//
// Returns one SocketProbe per discovered path.  The function is
// best-effort and never throws -- it logs at WARN level on partial
// failures and returns whatever it managed to scan.
std::vector<SocketProbe> sweep_stale_sockets();

} // namespace idiff::rpc

#endif // IDIFF_RPC_SOCKET_PATHS_H
