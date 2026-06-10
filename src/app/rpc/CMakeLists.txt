# idiff_rpc: in-process JSON-RPC 2.0 server.
#
# POSIX: Unix Domain Socket via standalone Asio.
# Windows: Named Pipe via standalone Asio + Win32 API.
# Linked into the idiff executable when IDIFF_HAVE_RPC is ON.  Carries
# the Asio + nlohmann/json deps and the dispatcher / method handlers.

if(WIN32)
    set(IDIFF_SOCKET_PATHS_SRC socket_paths_win32.cpp)
else()
    set(IDIFF_SOCKET_PATHS_SRC socket_paths.cpp)
endif()

add_library(idiff_rpc STATIC
    rpc_dispatcher.cpp
    rpc_server.cpp
    ${IDIFF_SOCKET_PATHS_SRC}
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

if(WIN32)
    # Asio on Windows may reference Winsock symbols even when only
    # stream_handle is used; linking ws2_32 keeps linkage clean.
    target_link_libraries(idiff_rpc PRIVATE ws2_32)
    # Standalone Asio requires _WIN32_WINNT >= 0x0601 (Windows 7+).
    # PRIVATE: do not lower the API level for consumers that may
    # already target a newer Windows version.
    target_compile_definitions(idiff_rpc PRIVATE _WIN32_WINNT=0x0601)
endif()

set_target_properties(idiff_rpc PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
