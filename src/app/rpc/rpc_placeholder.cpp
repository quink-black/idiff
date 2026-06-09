// Placeholder translation unit so the idiff_rpc static library has at
// least one object file.  Real RPC sources (rpc_dispatcher.cpp,
// rpc_methods.cpp, state_serializer.cpp, screenshot_composer.cpp,
// rpc_server.cpp) will be added in Phase 1 steps 2-6 and this file
// will be deleted at that point.
//
// Keeping the symbol exported (even though it is unused) ensures the
// archive is non-empty on platforms that warn about empty .a files.

namespace idiff::rpc {
extern const char* placeholder_marker();
const char* placeholder_marker() { return "idiff_rpc placeholder"; }
} // namespace idiff::rpc
