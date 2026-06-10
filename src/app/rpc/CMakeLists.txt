# idiff_rpc: in-process JSON-RPC 2.0 server.
#
# Phase 1: POSIX-only (Unix Domain Socket via standalone-asio).
# Linked into the idiff executable when IDIFF_HAVE_RPC is ON.  Carries
# the Asio + nlohmann/json deps and the dispatcher / method handlers.
#
# Currently a placeholder static library; sources will be added
# incrementally over Phase 1 steps 2-7.

add_library(idiff_rpc STATIC
    rpc_dispatcher.cpp
    rpc_server.cpp
)

target_include_directories(idiff_rpc PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

# Public deps so consumers (the idiff executable, and tests that compile
# rpc sources directly) get the include dirs without restating them.
target_link_libraries(idiff_rpc PUBLIC
    idiff_domain
    nlohmann_json::nlohmann_json
)

# Asio is private: only the .cpp implementations need its headers; the
# public interface uses Pimpl to keep Asio types out of headers.
target_link_libraries(idiff_rpc PRIVATE
    asio::asio
)

set_target_properties(idiff_rpc PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
