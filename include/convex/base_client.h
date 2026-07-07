#pragma once

// Synchronous, transport-free Convex sync client, ported from convex-rs's
// BaseConvexClient. It owns all protocol state; callers drive it by invoking
// methods (which enqueue outgoing messages) and by feeding decoded server
// messages into receive_message(). Callers must drain pop_next_message()
// after every operation. No I/O, no threads, no clocks — which makes every
// ordering rule below unit-testable.

#include <convex/error.h>
#include <convex/protocol.h>
#include <convex/value.h>

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace convex {

/// Identifies one subscriber of one query. Many subscribers can share a
/// query_id (identical function + args are deduplicated); the index is
/// globally unique and never reused.
struct subscriber_id {
    query_id query = 0;
    std::uint64_t index = 0;

    friend auto operator<=>(const subscriber_id&, const subscriber_id&) = default;
};

class base_client {
public:
    base_client() = default;

    // ------------------------------------------------------------------
    // Local operations (each may enqueue outgoing messages)
    // ------------------------------------------------------------------

    /// Subscribe to a query. Identical (path, args) pairs share one server
    /// subscription. The path is canonicalized internally.
    subscriber_id subscribe(std::string_view udf_path, value_object args);

    /// Drop one subscriber; the server subscription is removed when the last
    /// subscriber goes away. Unknown ids are ignored.
    void unsubscribe(const subscriber_id& id);

    /// Enqueue a mutation/action request; returns the request id used to
    /// correlate the completion event.
    request_id mutation(std::string_view udf_path, value_object args);
    request_id action(std::string_view udf_path, value_object args);

    /// Set (or clear, with auth_token::none()) the authentication token.
    void set_auth(auth_token token);

    // ------------------------------------------------------------------
    // Server input
    // ------------------------------------------------------------------

    struct receive_result {
        /// True when query results changed and a fresh snapshot should be
        /// delivered to subscribers.
        bool state_changed = false;
        /// Queries whose results changed in this message (updated, failed,
        /// or removed), for per-subscription update delivery.
        std::vector<query_id> changed_queries;
        /// Requests whose results became deliverable. Successful mutations
        /// appear here only once a Transition has advanced past their
        /// timestamp (read-your-writes); failed mutations and all actions
        /// complete immediately.
        std::vector<std::pair<request_id, function_result>> completed_requests;
        /// When set, the connection is broken (protocol violation, auth or
        /// fatal error): the caller must tear down the transport, then call
        /// restart() and reconnect.
        std::optional<std::string> reconnect_reason;
    };

    /// Feed one decoded server message.
    receive_result receive_message(const server_message& message);

    // ------------------------------------------------------------------
    // Outgoing queue
    // ------------------------------------------------------------------

    /// Pop the next message to send, if any. Drain after every operation.
    std::optional<client_message> pop_next_message();

    // ------------------------------------------------------------------
    // Reconnect
    // ------------------------------------------------------------------

    /// Rebuild protocol state after a transport failure. Clears the outgoing
    /// queue, then re-enqueues Authenticate (if any; pass a freshly fetched
    /// token to replace a possibly expired one), one ModifyQuerySet restoring
    /// every live query (with journals), and every in-flight mutation in
    /// original order. In-flight actions cannot be safely retried; they fail
    /// and are returned as completed (with an error result).
    /// The caller then sends make_connect_message() first on the new socket.
    [[nodiscard]] std::vector<std::pair<request_id, function_result>> restart(
        std::optional<auth_token> refreshed_auth = std::nullopt);

    /// Build the Connect message for a (re)connection attempt. Bumps the
    /// connection count.
    connect_message make_connect_message(std::string last_close_reason,
                                         std::optional<std::uint64_t> client_ts = std::nullopt);

    // ------------------------------------------------------------------
    // Introspection
    // ------------------------------------------------------------------

    /// Latest server result for a query, if one arrived. New subscribers to
    /// an existing query use this for their initial value.
    const function_result* latest_result(query_id id) const;

    /// All current query results, keyed by query id.
    const std::map<query_id, function_result>& latest_results() const {
        return remote_results_;
    }

    const std::string& session_id() const { return session_id_; }
    std::optional<timestamp> max_observed_timestamp() const { return max_observed_timestamp_; }

    /// True once every query re-sent by the last restart() has received a
    /// result — the signal that reconnect backoff can reset.
    bool has_synced_past_last_restart() const { return outstanding_queries_.empty(); }

private:
    struct local_query {
        query_id id = 0;
        std::string udf_path;    // canonical
        std::string args_json;   // canonical (sorted keys)
        std::size_t num_subscribers = 0;
        std::optional<std::string> journal;
    };

    struct pending_request {
        enum class kind : std::uint8_t { mutation, action };
        kind typ = kind::mutation;
        bool completed = false;               // mutation succeeded, awaiting watermark
        std::optional<timestamp> ts;          // success timestamp (mutations only)
        std::optional<function_result> result;
        client_message original_message;      // resent on reconnect (mutations only)
    };

    receive_result handle_transition(const transition_message& t);
    void apply_state_modification(const state_modification& mod);
    void enqueue_authenticate();
    void observe_timestamp(timestamp ts);
    /// Completed mutations whose ts <= watermark become deliverable.
    void flush_completed(timestamp watermark,
                         std::vector<std::pair<request_id, function_result>>& out);

    // Local (desired) state.
    std::string session_id_ = generate_session_id();
    query_id next_query_id_ = 0;
    std::uint64_t next_subscription_index_ = 0;
    query_set_version query_set_version_ = 0;
    identity_version identity_version_ = 0;
    std::map<std::string, local_query> query_set_;    // token -> query
    std::map<query_id, std::string> id_to_token_;
    std::optional<auth_token> auth_;

    // Remote (server-acknowledged) state.
    state_version remote_version_{};
    std::map<query_id, function_result> remote_results_;

    // In-flight mutations/actions.
    request_id next_request_id_ = 0;
    std::map<request_id, pending_request> ongoing_requests_;

    // Connection bookkeeping.
    std::uint32_t connection_count_ = 0;
    std::optional<timestamp> max_observed_timestamp_;
    std::set<query_id> outstanding_queries_;  // resent by restart(), awaiting results

    std::deque<client_message> outgoing_;
};

}  // namespace convex
