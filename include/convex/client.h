#pragma once

// The realtime Convex client: wires a base_client state machine to a
// websocket_transport, handling reconnection with jittered exponential
// backoff, server-inactivity detection, auth refresh, and callback delivery.
//
// Threading model. Transport callbacks and the internal timer thread mutate
// state under a lock; user callbacks are never invoked while it is held.
// Delivery is controlled by client_options::delivery_mode:
//   - pumped (default): callbacks are queued and fire only inside
//     process_events(), on the calling thread — ideal for game loops.
//   - immediate: callbacks fire on whatever internal thread produced them.

#include <convex/base_client.h>
#include <convex/error.h>
#include <convex/protocol.h>
#include <convex/transport.h>
#include <convex/value.h>
#include <convex/version.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace convex {

struct client_impl;

enum class connection_state : std::uint8_t { disconnected, connecting, connected };

/// Point-in-time snapshot of the connection for UX and telemetry
/// ("reconnecting, attempt 3, 2 commands in flight...").
struct connection_info {
    connection_state state = connection_state::disconnected;
    /// Consecutive failed/broken connection attempts since the last healthy
    /// sync; 0 while healthy.
    std::uint32_t retries = 0;
    std::size_t inflight_mutations = 0;
    std::size_t inflight_actions = 0;
    /// True once every query re-sent by the last reconnect has a result.
    bool has_synced_past_last_restart = false;
    /// Why the previous connection ended ("InitialConnect" before the first).
    std::string last_close_reason;
    /// Connect messages sent over this client's lifetime.
    std::uint32_t connection_count = 0;
};

/// Fetches an auth token. Called with force_refresh=true when reconnecting
/// (the previous token may have expired). Returning nullopt keeps the
/// current token. Called on an internal thread; must not block for long.
using auth_fetcher = std::function<std::optional<auth_token>(bool force_refresh)>;

struct client_options {
    /// Deployment URL, e.g. "https://happy-animal-123.convex.cloud".
    std::string deployment_url;

    /// Websocket implementation. Required.
    std::shared_ptr<websocket_transport> websocket;

    enum class delivery : std::uint8_t { pumped, immediate };
    delivery delivery_mode = delivery::pumped;

    /// Reported to the server in the Convex-Client header.
    std::string client_id = "cpp-" CONVEX_VERSION;

    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{15000};
    std::chrono::milliseconds server_inactivity_threshold{30000};
};

class client {
public:
    /// Creates the client and starts connecting. Throws std::invalid_argument
    /// on a malformed URL or missing transport.
    explicit client(client_options options);

    /// Closes the connection and stops all internal threads. Callbacks still
    /// queued for process_events() are delivered from the destructor (on the
    /// destroying thread); remaining pending mutation/action callbacks then
    /// complete with an error. Futures from the promise-based overloads are
    /// always satisfied, never broken. Subscription handles may safely
    /// outlive the client (their callbacks just stop).
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;

    // ------------------------------------------------------------------
    // Subscriptions
    // ------------------------------------------------------------------

    using update_callback = std::function<void(const function_result&)>;

    /// Move-only RAII handle; destroying it (or calling unsubscribe())
    /// removes this subscriber, and the server subscription once no
    /// subscribers remain.
    class subscription {
    public:
        subscription() = default;
        subscription(subscription&& other) noexcept;
        subscription& operator=(subscription&& other) noexcept;
        subscription(const subscription&) = delete;
        subscription& operator=(const subscription&) = delete;
        ~subscription();

        void unsubscribe();
        bool active() const { return impl_ != nullptr; }

    private:
        friend class client;
        subscription(std::shared_ptr<client_impl> impl, subscriber_id id)
            : impl_(std::move(impl)), id_(id) {}
        std::shared_ptr<client_impl> impl_;
        subscriber_id id_{};
    };

    /// Subscribe to a query. `on_update` fires with the current result as
    /// soon as one is known (immediately, if this deduplicates onto a query
    /// that already has one) and again on every change, including errors.
    [[nodiscard]] subscription subscribe(std::string_view udf_path, value_object args,
                                         update_callback on_update);

    // ------------------------------------------------------------------
    // One-shot operations
    // ------------------------------------------------------------------

    using result_callback = std::function<void(function_result)>;

    /// Run a mutation. Ordered: the callback fires only after the mutation's
    /// effects are reflected in subscribed query results (read-your-writes).
    void mutation(std::string_view udf_path, value_object args, result_callback on_done);
    std::future<function_result> mutation(std::string_view udf_path, value_object args);

    /// Run an action. Completes as soon as the server responds; fails if the
    /// connection drops while it is in flight (actions are not idempotent
    /// and are never retried).
    void action(std::string_view udf_path, value_object args, result_callback on_done);
    std::future<function_result> action(std::string_view udf_path, value_object args);

    /// Run a query once: subscribes, takes the first result, unsubscribes.
    void query(std::string_view udf_path, value_object args, result_callback on_done);
    std::future<function_result> query(std::string_view udf_path, value_object args);

    // ------------------------------------------------------------------
    // Auth / connection
    // ------------------------------------------------------------------

    /// Set or clear (auth_token::none()) the auth token, and install an
    /// optional fetcher used to refresh it on reconnects.
    void set_auth(auth_token token, auth_fetcher fetcher = nullptr);

    connection_state state() const;

    /// Point-in-time connection snapshot (state, retries, in-flight counts,
    /// last close reason).
    connection_info info() const;

    /// Observe connection state changes (delivered like other callbacks).
    void on_state_change(std::function<void(connection_state)> listener);

    /// Observe terminal authentication failure: the server rejected a token
    /// that was already refreshed (or refreshing is impossible), so the
    /// client stopped re-authenticating and continues unauthenticated.
    /// Call set_auth() with a new token to try again. Delivered like other
    /// callbacks.
    void on_auth_failure(std::function<void(std::string reason)> listener);

    /// Observe server-side console output (`console.log` in queries,
    /// mutations, actions), attributed to the emitting function. Delivered
    /// like other callbacks.
    void on_log_lines(std::function<void(const log_entry&)> listener);

    // ------------------------------------------------------------------
    // Event pump
    // ------------------------------------------------------------------

    /// Deliver queued callbacks on the calling thread (pumped mode). Returns
    /// the number of callbacks invoked. No-op in immediate mode.
    std::size_t process_events();

private:
    std::shared_ptr<client_impl> impl_;
};

/// Derive the websocket sync URL from a deployment URL:
/// https://x.convex.cloud -> wss://x.convex.cloud/api/sync.
/// Throws std::invalid_argument on unsupported schemes.
std::string deployment_to_ws_url(std::string_view deployment_url);

}  // namespace convex
