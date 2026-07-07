#include <convex/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace convex {

namespace {

std::uint64_t wall_clock_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

std::string deployment_to_ws_url(std::string_view deployment_url) {
    const auto scheme_end = deployment_url.find("://");
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument("convex: deployment URL has no scheme: " +
                                    std::string(deployment_url));
    }
    std::string scheme;
    for (const char c : deployment_url.substr(0, scheme_end)) {
        scheme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    std::string ws_scheme;
    if (scheme == "https" || scheme == "wss") {
        ws_scheme = "wss";
    } else if (scheme == "http" || scheme == "ws") {
        ws_scheme = "ws";
    } else {
        throw std::invalid_argument("convex: unsupported deployment URL scheme: " + scheme);
    }
    std::string rest(deployment_url.substr(scheme_end + 3));
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.empty()) {
        throw std::invalid_argument("convex: deployment URL has no host");
    }
    return ws_scheme + "://" + rest + "/api/sync";
}

// ===========================================================================

struct client_impl : std::enable_shared_from_this<client_impl> {
    using dispatch_list = std::vector<std::function<void()>>;

    // Per-connection observer. Holds the impl alive so late transport
    // callbacks are always safe; a stale generation makes them no-ops.
    struct slot final : websocket_observer {
        std::shared_ptr<client_impl> impl;
        std::uint64_t gen;
        slot(std::shared_ptr<client_impl> i, std::uint64_t g) : impl(std::move(i)), gen(g) {}
        void on_open() override { impl->ws_open(gen); }
        void on_message(std::string text) override { impl->ws_message(gen, std::move(text)); }
        void on_close(std::string reason) override { impl->ws_close(gen, std::move(reason)); }
    };

    explicit client_impl(client_options o) : opts(std::move(o)) {
        if (!opts.websocket) throw std::invalid_argument("convex: websocket transport required");
        ws_url = deployment_to_ws_url(opts.deployment_url);
        ws_headers = {{"Convex-Client", opts.client_id}};
    }

    // ------------------------------------------------------------------
    // Configuration (immutable after construction)
    // ------------------------------------------------------------------
    client_options opts;
    std::string ws_url;
    std::map<std::string, std::string> ws_headers;

    // ------------------------------------------------------------------
    // State guarded by `mu`
    // ------------------------------------------------------------------
    mutable std::mutex mu;
    base_client base;
    std::unique_ptr<websocket_connection> conn;
    std::unique_ptr<slot> conn_slot;
    std::uint64_t conn_gen = 0;
    bool socket_open = false;
    bool pending_open = false;  // on_open arrived before connect() returned
    connection_state state = connection_state::disconnected;
    std::string last_close_reason = "InitialConnect";
    auth_fetcher fetcher;
    std::uint32_t retries = 0;
    bool stopping = false;

    struct sub_entry {
        std::uint64_t index;
        client::update_callback cb;
    };
    std::map<query_id, std::vector<sub_entry>> subs;
    std::map<request_id, client::result_callback> pending;
    std::vector<std::function<void(connection_state)>> state_listeners;

    std::deque<std::function<void()>> event_queue;

    // Worker thread: reconnect timing, inactivity checks, deferred
    // destruction of dead connections (they cannot be destroyed from their
    // own callback thread).
    std::thread worker;
    std::condition_variable cv;
    bool reconnect_scheduled = false;
    std::chrono::steady_clock::time_point reconnect_at{};
    std::chrono::steady_clock::time_point last_server_msg{};
    std::vector<std::pair<std::unique_ptr<websocket_connection>, std::unique_ptr<slot>>> garbage;
    std::mt19937_64 rng{std::random_device{}()};

    // ------------------------------------------------------------------

    void start() {
        {
            std::lock_guard lk(mu);
            reconnect_scheduled = true;
            reconnect_at = std::chrono::steady_clock::now();
        }
        worker = std::thread([self = shared_from_this()] { self->worker_loop(); });
    }

    // Deliver produced events: run now (immediate mode) or enqueue for
    // process_events(). Must be called WITHOUT holding `mu`.
    void deliver(dispatch_list&& list) {
        if (list.empty()) return;
        if (opts.delivery_mode == client_options::delivery::immediate) {
            for (auto& fn : list) fn();
        } else {
            std::lock_guard lk(mu);
            for (auto& fn : list) event_queue.push_back(std::move(fn));
        }
    }

    void push_state_event(dispatch_list& out, connection_state s) {
        for (const auto& listener : state_listeners) {
            out.push_back([listener, s] { listener(s); });
        }
    }

    // Requires `mu`. Sends everything the state machine queued.
    void drain_outgoing() {
        if (!socket_open || !conn) return;
        while (auto msg = base.pop_next_message()) {
            conn->send_text(encode_client_message(*msg));
        }
    }

    std::chrono::milliseconds backoff_delay(std::uint32_t attempt) {
        const double base_ms = static_cast<double>(opts.initial_backoff.count());
        const double capped =
            std::min(base_ms * std::pow(2.0, static_cast<double>(attempt)),
                     static_cast<double>(opts.max_backoff.count()));
        std::uniform_real_distribution<double> jitter(0.5, 1.5);
        return std::chrono::milliseconds(static_cast<std::int64_t>(capped * jitter(rng)));
    }

    // Requires `mu`. Tears down the current connection (deferring its
    // destruction to the worker), rebuilds protocol state, schedules the
    // next attempt.
    void begin_disconnect(dispatch_list& out, std::string reason) {
        ++conn_gen;
        if (conn || conn_slot) {
            garbage.emplace_back(std::move(conn), std::move(conn_slot));
        }
        socket_open = false;
        pending_open = false;
        if (state != connection_state::disconnected) {
            state = connection_state::disconnected;
            push_state_event(out, state);
        }
        last_close_reason = std::move(reason);

        std::optional<auth_token> refreshed;
        if (fetcher) refreshed = fetcher(/*force_refresh=*/true);
        auto failed_actions = base.restart(std::move(refreshed));
        for (auto& [rid, result] : failed_actions) {
            if (auto it = pending.find(rid); it != pending.end()) {
                out.push_back([cb = std::move(it->second), r = std::move(result)]() mutable {
                    cb(std::move(r));
                });
                pending.erase(it);
            }
        }

        reconnect_scheduled = true;
        reconnect_at = std::chrono::steady_clock::now() + backoff_delay(retries);
        ++retries;
        cv.notify_all();
    }

    // Requires `mu`. Socket is open: send Connect, then the rebuilt queue.
    void handle_open(dispatch_list& out) {
        last_server_msg = std::chrono::steady_clock::now();
        state = connection_state::connected;
        push_state_event(out, state);
        conn->send_text(
            encode_client_message(base.make_connect_message(last_close_reason, wall_clock_ms())));
        drain_outgoing();
    }

    // ------------------------------------------------------------------
    // Transport callbacks (any thread)
    // ------------------------------------------------------------------

    void ws_open(std::uint64_t gen) {
        dispatch_list out;
        {
            std::lock_guard lk(mu);
            if (stopping || gen != conn_gen) return;
            socket_open = true;
            if (!conn) {
                // connect() has not returned yet; handle_open runs there.
                pending_open = true;
                return;
            }
            handle_open(out);
            cv.notify_all();  // worker must arm the inactivity timer
        }
        deliver(std::move(out));
    }

    void ws_message(std::uint64_t gen, std::string text) {
        dispatch_list out;
        {
            std::lock_guard lk(mu);
            if (stopping || gen != conn_gen) return;
            last_server_msg = std::chrono::steady_clock::now();

            server_message msg;
            try {
                msg = decode_server_message(text);
            } catch (const protocol_error& e) {
                begin_disconnect(out, std::string("ProtocolError: ") + e.what());
                deliver_later(std::move(out));
                return;
            }

            auto r = base.receive_message(msg);
            if (r.reconnect_reason) {
                begin_disconnect(out, *r.reconnect_reason);
            } else {
                for (const query_id qid : r.changed_queries) {
                    const function_result* fr = base.latest_result(qid);
                    if (fr == nullptr) continue;  // removed
                    const auto sub_it = subs.find(qid);
                    if (sub_it == subs.end()) continue;
                    for (const sub_entry& entry : sub_it->second) {
                        out.push_back([cb = entry.cb, snapshot = *fr] { cb(snapshot); });
                    }
                }
                for (auto& [rid, result] : r.completed_requests) {
                    if (auto it = pending.find(rid); it != pending.end()) {
                        out.push_back(
                            [cb = std::move(it->second), res = std::move(result)]() mutable {
                                cb(std::move(res));
                            });
                        pending.erase(it);
                    }
                }
                if (r.state_changed && base.has_synced_past_last_restart()) {
                    retries = 0;  // healthy again: reset backoff
                }
                drain_outgoing();
            }
        }
        deliver(std::move(out));
    }

    void ws_close(std::uint64_t gen, std::string reason) {
        dispatch_list out;
        {
            std::lock_guard lk(mu);
            if (stopping || gen != conn_gen) return;
            begin_disconnect(out, std::move(reason));
        }
        deliver(std::move(out));
    }

    // Helper for paths that already hold `mu` via lock_guard scoping quirks.
    void deliver_later(dispatch_list&& out) {
        // Called with `mu` held; queue directly, or hand to a detached
        // invoker for immediate mode.
        if (opts.delivery_mode == client_options::delivery::immediate) {
            // Rare path (protocol error). Run after the lock releases via
            // the event queue drained by the worker's next pass; to keep
            // immediate semantics, spawn a one-shot dispatcher.
            auto list = std::make_shared<dispatch_list>(std::move(out));
            std::thread([list] {
                for (auto& fn : *list) fn();
            }).detach();
        } else {
            for (auto& fn : out) event_queue.push_back(std::move(fn));
        }
    }

    // ------------------------------------------------------------------
    // Worker
    // ------------------------------------------------------------------

    void worker_loop() {
        std::unique_lock lk(mu);
        while (!stopping) {
            // Collect deferred-destruction work first.
            if (!garbage.empty()) {
                auto dead = std::move(garbage);
                garbage.clear();
                lk.unlock();
                dead.clear();  // joins transport threads outside the lock
                lk.lock();
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            std::optional<std::chrono::steady_clock::time_point> deadline;

            if (reconnect_scheduled) {
                if (now >= reconnect_at) {
                    reconnect_scheduled = false;
                    dispatch_list out;
                    attempt_connect(lk, out);
                    lk.unlock();
                    deliver(std::move(out));
                    lk.lock();
                    continue;
                }
                deadline = reconnect_at;
            }

            if (socket_open) {
                const auto inactive_at = last_server_msg + opts.server_inactivity_threshold;
                if (now >= inactive_at) {
                    dispatch_list out;
                    begin_disconnect(out, "InactiveServer");
                    lk.unlock();
                    deliver(std::move(out));
                    lk.lock();
                    continue;
                }
                if (!deadline || inactive_at < *deadline) deadline = inactive_at;
            }

            if (deadline) {
                cv.wait_until(lk, *deadline);
            } else {
                cv.wait(lk);
            }
        }
    }

    // Requires `lk` locked on entry and exit; unlocks around the transport
    // call (it may invoke observer callbacks synchronously).
    void attempt_connect(std::unique_lock<std::mutex>& lk, dispatch_list& out) {
        state = connection_state::connecting;
        push_state_event(out, state);
        const std::uint64_t gen = ++conn_gen;
        auto new_slot = std::make_unique<slot>(shared_from_this(), gen);
        slot* observer = new_slot.get();

        lk.unlock();
        std::unique_ptr<websocket_connection> new_conn;
        std::string failure;
        try {
            new_conn = opts.websocket->connect(ws_url, ws_headers, *observer);
        } catch (const std::exception& e) {
            failure = e.what();
        }
        lk.lock();

        if (stopping || gen != conn_gen) {
            // Superseded while unlocked; discard via the garbage list.
            if (new_conn) garbage.emplace_back(std::move(new_conn), std::move(new_slot));
            return;
        }
        if (!new_conn) {
            conn_slot = std::move(new_slot);  // keep alive until generation retires
            begin_disconnect(out, failure.empty() ? "ConnectFailed" : "ConnectFailed: " + failure);
            return;
        }
        conn = std::move(new_conn);
        conn_slot = std::move(new_slot);
        if (pending_open) {
            pending_open = false;
            socket_open = true;
            handle_open(out);
        }
        cv.notify_all();
    }

    // ------------------------------------------------------------------
    // Public operation bodies
    // ------------------------------------------------------------------

    subscriber_id do_subscribe(std::string_view path, value_object args,
                               client::update_callback cb) {
        dispatch_list out;
        subscriber_id sid;
        {
            std::lock_guard lk(mu);
            sid = base.subscribe(path, std::move(args));
            if (const function_result* fr = base.latest_result(sid.query)) {
                out.push_back([cb, snapshot = *fr] { cb(snapshot); });
            }
            subs[sid.query].push_back(sub_entry{sid.index, std::move(cb)});
            drain_outgoing();
        }
        deliver(std::move(out));
        return sid;
    }

    void do_unsubscribe(const subscriber_id& sid) {
        std::lock_guard lk(mu);
        if (stopping) return;
        if (const auto it = subs.find(sid.query); it != subs.end()) {
            auto& entries = it->second;
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [&](const sub_entry& e) { return e.index == sid.index; }),
                          entries.end());
            if (entries.empty()) subs.erase(it);
        }
        base.unsubscribe(sid);
        drain_outgoing();
    }

    void do_request(bool is_mutation, std::string_view path, value_object args,
                    client::result_callback cb) {
        {
            std::lock_guard lk(mu);
            if (stopping) {
                // Complete outside the lock below.
            } else {
                const request_id rid = is_mutation ? base.mutation(path, std::move(args))
                                                   : base.action(path, std::move(args));
                pending.emplace(rid, std::move(cb));
                drain_outgoing();
                return;
            }
        }
        cb(function_result::error("convex: client is shut down"));
    }

    void do_set_auth(auth_token token, auth_fetcher f) {
        std::lock_guard lk(mu);
        if (stopping) return;
        fetcher = std::move(f);
        base.set_auth(std::move(token));
        drain_outgoing();
    }

    std::size_t do_process_events() {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard lk(mu);
            batch.swap(event_queue);
        }
        for (auto& fn : batch) fn();
        return batch.size();
    }

    void shutdown() {
        std::vector<client::result_callback> orphaned;
        std::unique_ptr<websocket_connection> dead_conn;
        std::unique_ptr<slot> dead_slot;
        {
            std::lock_guard lk(mu);
            if (stopping) return;
            stopping = true;
            ++conn_gen;
            dead_conn = std::move(conn);
            dead_slot = std::move(conn_slot);
            socket_open = false;
            for (auto& [rid, cb] : pending) orphaned.push_back(std::move(cb));
            pending.clear();
            subs.clear();
            cv.notify_all();
        }
        if (worker.joinable()) worker.join();
        dead_conn.reset();  // outside the lock: may join transport threads
        dead_slot.reset();
        {
            // Anything the worker parked before stopping.
            std::lock_guard lk(mu);
            garbage.clear();
        }
        for (auto& cb : orphaned) cb(function_result::error("convex: client is shut down"));
    }
};

// ===========================================================================
// client facade
// ===========================================================================

client::client(client_options options) : impl_(std::make_shared<client_impl>(std::move(options))) {
    impl_->start();
}

client::~client() {
    if (impl_) impl_->shutdown();
}

client::subscription::subscription(subscription&& other) noexcept
    : impl_(std::move(other.impl_)), id_(other.id_) {}

client::subscription& client::subscription::operator=(subscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        impl_ = std::move(other.impl_);
        id_ = other.id_;
    }
    return *this;
}

client::subscription::~subscription() { unsubscribe(); }

void client::subscription::unsubscribe() {
    if (impl_) {
        impl_->do_unsubscribe(id_);
        impl_.reset();
    }
}

client::subscription client::subscribe(std::string_view udf_path, value_object args,
                                       update_callback on_update) {
    const subscriber_id sid = impl_->do_subscribe(udf_path, std::move(args), std::move(on_update));
    return subscription(impl_, sid);
}

void client::mutation(std::string_view udf_path, value_object args, result_callback on_done) {
    impl_->do_request(true, udf_path, std::move(args), std::move(on_done));
}

std::future<function_result> client::mutation(std::string_view udf_path, value_object args) {
    auto promise = std::make_shared<std::promise<function_result>>();
    auto future = promise->get_future();
    mutation(udf_path, std::move(args),
             [promise](function_result r) { promise->set_value(std::move(r)); });
    return future;
}

void client::action(std::string_view udf_path, value_object args, result_callback on_done) {
    impl_->do_request(false, udf_path, std::move(args), std::move(on_done));
}

std::future<function_result> client::action(std::string_view udf_path, value_object args) {
    auto promise = std::make_shared<std::promise<function_result>>();
    auto future = promise->get_future();
    action(udf_path, std::move(args),
           [promise](function_result r) { promise->set_value(std::move(r)); });
    return future;
}

void client::query(std::string_view udf_path, value_object args, result_callback on_done) {
    // One-shot: subscribe, deliver the first result, then unsubscribe. The
    // handle lives inside the callback; delivery always runs a copy of the
    // callback, so releasing the original from within it is safe.
    auto holder = std::make_shared<subscription>();
    auto fired = std::make_shared<std::atomic_bool>(false);
    *holder = subscribe(udf_path, std::move(args),
                        [holder, fired, cb = std::move(on_done)](const function_result& fr) {
                            if (fired->exchange(true)) return;
                            cb(fr);
                            holder->unsubscribe();
                        });
}

std::future<function_result> client::query(std::string_view udf_path, value_object args) {
    auto promise = std::make_shared<std::promise<function_result>>();
    auto future = promise->get_future();
    query(udf_path, std::move(args),
          [promise](function_result r) { promise->set_value(std::move(r)); });
    return future;
}

void client::set_auth(auth_token token, auth_fetcher fetcher) {
    impl_->do_set_auth(std::move(token), std::move(fetcher));
}

connection_state client::state() const {
    std::lock_guard lk(impl_->mu);
    return impl_->state;
}

void client::on_state_change(std::function<void(connection_state)> listener) {
    std::lock_guard lk(impl_->mu);
    impl_->state_listeners.push_back(std::move(listener));
}

std::size_t client::process_events() { return impl_->do_process_events(); }

}  // namespace convex
