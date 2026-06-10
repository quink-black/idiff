// Helpers for reasoning about /tmp/idiff-*.sock:
//
//   * compose_socket_path(pid)       -- "/tmp/idiff-<pid>.sock"
//   * compose_identity_label(pid)    -- "idiff:<pid>"  (UI chip + window title)
//   * sweep_stale_sockets()          -- enumerates /tmp/idiff-*.sock, probes
//                                       each by connect(); removes the entry
//                                       if the kernel reports ECONNREFUSED
//                                       (no listener), leaves it untouched
//                                       on EACCES/ETIMEDOUT/etc.
//
// Why this lives in the rpc/ subtree rather than App: the same logic is
// useful to any future POSIX-only callers (e.g. an idiff CLI helper)
// and has no dependency on App / SDL / OpenCV.

#ifndef IDIFF_RPC_SOCKET_PATHS_H
#define IDIFF_RPC_SOCKET_PATHS_H

#include <cstddef>
#include <string>
#include <vector>

namespace idiff::rpc {

// "/tmp/idiff-<pid>.sock"
std::string compose_socket_path(int pid);

// "idiff:<pid>" -- the user-facing identity tag mirrored in the window
// title and status-bar chip so the user can correlate a GUI window with
// a given /tmp/idiff-*.sock.
std::string compose_identity_label(int pid);

// One result of sweep_stale_sockets().
struct SocketProbe {
    std::string path;        // full /tmp/idiff-<pid>.sock
    int pid = -1;            // parsed from the filename, -1 if unparseable
    bool alive = false;      // connect() succeeded -> a listener owns it
    bool removed = false;    // we unlinked it during sweep (was stale)
};

// Find every /tmp/idiff-*.sock, classify each (alive vs stale), and
// remove the stale ones.  "Stale" means connect() returned
// ECONNREFUSED -- the path exists but no process is listening; this
// happens after a hard kill / crash where the socket file outlived the
// idiff process.  Sockets that fail with EACCES, ETIMEDOUT, or other
// errors are left alone (could be someone else's, or transient).
//
// Returns one SocketProbe per discovered path.  The function is
// best-effort and never throws -- it logs at WARN level on partial
// failures and returns whatever it managed to scan.
std::vector<SocketProbe> sweep_stale_sockets();

} // namespace idiff::rpc

#endif // IDIFF_RPC_SOCKET_PATHS_H
