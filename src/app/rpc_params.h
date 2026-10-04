#ifndef IDIFF_APP_RPC_PARAMS_H
#define IDIFF_APP_RPC_PARAMS_H

// Parameter validation for the JSON-RPC method handlers.
//
// Every function throws rpc::RpcException with ErrorCode::InvalidParams
// when the input does not satisfy it, and the message names the field
// at fault.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace idiff {

struct ImageEntry;

namespace rpc_params {

const nlohmann::json& require_object(const nlohmann::json& params);

int require_int_field(const nlohmann::json& params, const char* key);

const std::string& require_string_field(const nlohmann::json& params,
                                        const char* key);

void check_index(int idx, std::size_t size, const char* what);

// Resolves an entry reference to an index into `entries`.  An integer
// is an index.  A string names the entries whose ImageEntry::path
// equals it; when none does, the entries whose path resolves to the
// same file once symlinks and "." / ".." are resolved.  Exactly one
// entry must match.  The library does not deduplicate paths, so a path
// that several entries share is rejected and the message lists their
// indices.
int resolve_entry(const nlohmann::json& ref,
                  const std::vector<ImageEntry>& entries,
                  const char* what);

// Resolves each element of the array `refs` with resolve_entry().
std::vector<int> resolve_entries(const nlohmann::json& refs,
                                 const std::vector<ImageEntry>& entries,
                                 const char* what);

// Resolves the entry named by exactly one of params["index"] (integer)
// or params["path"] (string).
int require_entry_field(const nlohmann::json& params,
                        const std::vector<ImageEntry>& entries);

} // namespace rpc_params
} // namespace idiff

#endif // IDIFF_APP_RPC_PARAMS_H
