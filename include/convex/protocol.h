#pragma once

// Wire-protocol message types for the Convex WebSocket sync protocol
// (wss://<deployment>/api/sync). Ported from convex-rs's convex_sync_types
// crate; JSON shapes follow convex-js src/browser/sync/protocol.ts, the
// canonical client implementation.

#include <convex/error.h>
#include <convex/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace convex {

/// Thrown when a server message cannot be decoded.
class protocol_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Server timestamp: opaque 64-bit token, base64 of 8 little-endian bytes on
/// the wire. Never a double — values exceed 2^53.
using timestamp = std::uint64_t;

using query_id = std::uint32_t;
using query_set_version = std::uint32_t;
using identity_version = std::uint32_t;
using request_id = std::uint32_t;

/// Canonicalize a UDF path: "module/path:function"; a missing function part
/// becomes "default"; a trailing ".js" on the module is stripped.
std::string canonicalize_udf_path(std::string_view path);

/// Serialize named function arguments to their wire form: a single-element
/// JSON array holding the args object. Because value_object keys are sorted,
/// the result is canonical and doubles as the query-identity token component.
std::string serialize_args(const value_object& args);

// --------------------------------------------------------------------------
// Client -> server messages
// --------------------------------------------------------------------------

struct connect_message {
    std::string session_id;         // UUID v4, stable per client instance
    std::uint32_t connection_count = 0;
    std::string last_close_reason = "InitialConnect";
    std::optional<timestamp> max_observed_timestamp;
    std::optional<std::uint64_t> client_ts;  // wall clock ms, for skew measurement
};

struct query_add {
    query_id id = 0;
    std::string udf_path;                     // canonicalized
    std::string args_json;                    // from serialize_args()
    std::optional<std::string> journal;       // opaque QueryJournal
    std::optional<std::string> component_path;
};

struct query_remove {
    query_id id = 0;
};

using query_set_modification = std::variant<query_add, query_remove>;

struct modify_query_set_message {
    query_set_version base_version = 0;
    query_set_version new_version = 0;
    std::vector<query_set_modification> modifications;
};

struct mutation_request_message {
    request_id id = 0;
    std::string udf_path;
    std::string args_json;
    std::optional<std::string> component_path;
};

struct action_request_message {
    request_id id = 0;
    std::string udf_path;
    std::string args_json;
    std::optional<std::string> component_path;
};

/// Authentication token. none clears auth; user carries an OIDC JWT; admin
/// carries a deploy key and may impersonate a user identity.
struct auth_token {
    enum class kind : std::uint8_t { none, user, admin };
    kind type = kind::none;
    std::string value;                          // JWT or deploy key
    std::optional<value_object> acting_as;      // admin impersonation attributes

    static auth_token none() { return {}; }
    static auth_token user(std::string jwt) {
        return {kind::user, std::move(jwt), std::nullopt};
    }
    static auth_token admin(std::string deploy_key,
                            std::optional<value_object> acting_as = std::nullopt) {
        return {kind::admin, std::move(deploy_key), std::move(acting_as)};
    }
};

struct authenticate_message {
    identity_version base_version = 0;
    auth_token token;
};

using client_message = std::variant<connect_message, authenticate_message,
                                    modify_query_set_message, mutation_request_message,
                                    action_request_message>;

/// Encode a client message to its wire JSON text.
std::string encode_client_message(const client_message& message);

// --------------------------------------------------------------------------
// Server -> client messages
// --------------------------------------------------------------------------

struct state_version {
    query_set_version query_set = 0;
    identity_version identity = 0;
    timestamp ts = 0;

    friend bool operator==(const state_version&, const state_version&) = default;
};

struct query_updated {
    query_id id = 0;
    value result;
    std::vector<std::string> log_lines;
    std::optional<std::string> journal;
};

struct query_failed {
    query_id id = 0;
    std::string error_message;
    std::optional<value> error_data;  // present when raised via ConvexError
    std::vector<std::string> log_lines;
    std::optional<std::string> journal;
};

struct query_removed {
    query_id id = 0;
};

using state_modification = std::variant<query_updated, query_failed, query_removed>;

struct transition_message {
    state_version start_version;
    state_version end_version;
    std::vector<state_modification> modifications;
};

struct mutation_response_message {
    request_id id = 0;
    function_result result = function_result::error("uninitialized");
    std::optional<timestamp> ts;  // present on success only
    std::vector<std::string> log_lines;
};

struct action_response_message {
    request_id id = 0;
    function_result result = function_result::error("uninitialized");
    std::vector<std::string> log_lines;
};

struct auth_error_message {
    std::string error;
    std::optional<identity_version> base_version;
    bool auth_update_attempted = false;
};

struct fatal_error_message {
    std::string error;
};

struct ping_message {};

/// One slice of a Transition too large for a single frame (> 5 MB). Chunks
/// are consecutive substrings of the serialized Transition JSON, sent in
/// order; transition_id identifies one split so mixed sequences can be
/// detected. The server only splits for clients that advertise support
/// (currently npm >= a minimum version, keyed off the client-version path
/// segment of the sync URL), but decoding and reassembly are implemented so
/// a chunk never kills the connection.
struct transition_chunk_message {
    std::string chunk;
    std::uint32_t part_number = 0;  // 0-based
    std::uint32_t total_parts = 0;
    std::string transition_id;
};

using server_message =
    std::variant<transition_message, mutation_response_message, action_response_message,
                 auth_error_message, fatal_error_message, ping_message,
                 transition_chunk_message>;

/// Decode a server wire JSON text. Throws protocol_error on malformed input
/// or unknown message types.
server_message decode_server_message(std::string_view json_text);

/// Reassembles TransitionChunk slices into a full Transition. Per-connection
/// state: feed every chunk as it arrives; a non-chunk message other than Ping
/// arriving mid-sequence means the buffer is stale (call abandon()), and the
/// buffer never survives a reconnect. Mirrors convex-js's assembleTransition.
class transition_chunk_assembler {
public:
    /// Consume one chunk. Returns the assembled Transition when the final
    /// part arrives, std::nullopt while parts are missing. Throws
    /// protocol_error — after discarding the buffer — on inconsistent or
    /// out-of-order parts, or when the assembled document is not a
    /// Transition.
    std::optional<transition_message> feed(const transition_chunk_message& chunk);

    /// Discard any partial buffer (interleaved message or reconnect).
    void abandon();

    /// True while a split Transition is partially received.
    bool buffering() const { return total_parts_ != 0; }

private:
    std::string data_;
    std::uint32_t received_parts_ = 0;
    std::uint32_t total_parts_ = 0;  // 0 = idle
    std::string transition_id_;
};

// --------------------------------------------------------------------------

/// Generate a random UUID v4 string (hyphenated), used as the session id.
std::string generate_session_id();

}  // namespace convex
