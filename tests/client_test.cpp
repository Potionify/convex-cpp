// Runtime client tests against the in-memory mock transport: connect
// handshake, subscription delivery via the event pump, mutation ordering,
// reconnect rebuild, action-failure semantics, inactivity detection.

#include <chrono>
#include <future>
#include <thread>

#include <convex/client.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "mock_transport.h"

using namespace convex;
using convex::testing::mock_transport;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

client_options make_options(std::shared_ptr<mock_transport> transport,
                            client_options::delivery mode = client_options::delivery::pumped) {
    client_options o;
    o.deployment_url = "https://unit-test.convex.cloud";
    o.websocket = std::move(transport);
    o.delivery_mode = mode;
    o.initial_backoff = 10ms;   // keep reconnect tests fast
    o.max_backoff = 50ms;
    o.server_inactivity_threshold = 60s;  // effectively off unless a test lowers it
    return o;
}

template <typename Pred>
bool pump_until(client& c, Pred pred, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        c.process_events();
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

// Base64 little-endian encodings of small timestamps used below.
constexpr const char* TS0 = "AAAAAAAAAAA=";
constexpr const char* TS5 = "BQAAAAAAAAA=";
constexpr const char* TS6 = "BgAAAAAAAAA=";
constexpr const char* TS10 = "CgAAAAAAAAA=";

std::string transition_updated(int start_qs, const char* start_ts, int end_qs, const char* end_ts,
                               int query, const std::string& value_json) {
    return R"({"type":"Transition","startVersion":{"querySet":)" + std::to_string(start_qs) +
           R"(,"identity":0,"ts":")" + start_ts + R"("},"endVersion":{"querySet":)" +
           std::to_string(end_qs) + R"(,"identity":0,"ts":")" + end_ts +
           R"("},"modifications":[{"type":"QueryUpdated","queryId":)" + std::to_string(query) +
           R"(,"value":)" + value_json + R"(,"logLines":[]}]})";
}

std::string transition_empty(int qs, const char* start_ts, const char* end_ts) {
    return R"({"type":"Transition","startVersion":{"querySet":)" + std::to_string(qs) +
           R"(,"identity":0,"ts":")" + start_ts + R"("},"endVersion":{"querySet":)" +
           std::to_string(qs) + R"(,"identity":0,"ts":")" + end_ts + R"("},"modifications":[]})";
}

}  // namespace

TEST(Client, ConnectHandshakeAndHeaders) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));

    ASSERT_TRUE(transport->wait_for_attempts(1));
    EXPECT_EQ(transport->sent(0).size(), 0u);  // nothing before open
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 1));

    const json connect = json::parse(transport->sent(0)[0]);
    EXPECT_EQ(connect["type"], "Connect");
    EXPECT_EQ(connect["connectionCount"], 0);
    EXPECT_EQ(connect["lastCloseReason"], "InitialConnect");
    EXPECT_EQ(connect["sessionId"].get<std::string>().size(), 36u);

    EXPECT_EQ(transport->url(0), "wss://unit-test.convex.cloud/api/sync");
    const auto headers = transport->headers(0);
    ASSERT_TRUE(headers.contains("Convex-Client"));
    EXPECT_EQ(headers.at("Convex-Client"), "cpp-0.1.0");
}

TEST(Client, SubscribeDeliversUpdatesThroughPump) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));

    std::vector<function_result> updates;
    auto sub = c.subscribe("messages:list", {{"channel", value("general")}},
                           [&](const function_result& r) { updates.push_back(r); });

    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 2));  // Connect + ModifyQuerySet

    const json mqs = json::parse(transport->sent(0)[1]);
    EXPECT_EQ(mqs["type"], "ModifyQuerySet");
    EXPECT_EQ(mqs["baseVersion"], 0);
    EXPECT_EQ(mqs["newVersion"], 1);
    EXPECT_EQ(mqs["modifications"][0]["type"], "Add");
    EXPECT_EQ(mqs["modifications"][0]["queryId"], 0);
    EXPECT_EQ(mqs["modifications"][0]["udfPath"], "messages:list");

    transport->server_send(0, transition_updated(0, TS0, 1, TS5, 0, R"({"$integer":"KgAAAAAAAAA="})"));
    ASSERT_TRUE(pump_until(c, [&] { return !updates.empty(); }));
    ASSERT_TRUE(updates[0].ok());
    EXPECT_EQ(updates[0].get_value(), value(std::int64_t{42}));

    // Identical second subscription: shares the query, gets the cached value
    // immediately, no extra wire traffic.
    std::vector<function_result> second;
    auto sub2 = c.subscribe("messages:list", {{"channel", value("general")}},
                            [&](const function_result& r) { second.push_back(r); });
    ASSERT_TRUE(pump_until(c, [&] { return !second.empty(); }));
    EXPECT_EQ(second[0].get_value(), value(std::int64_t{42}));
    EXPECT_EQ(transport->sent(0).size(), 2u);
}

TEST(Client, MutationResultWaitsForTransitionWatermark) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));
    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 1));

    auto future = c.mutation("messages:send", {{"body", value("hi")}});
    ASSERT_TRUE(transport->wait_for_sent(0, 2));
    const json mut = json::parse(transport->sent(0)[1]);
    EXPECT_EQ(mut["type"], "Mutation");
    EXPECT_EQ(mut["requestId"], 0);

    transport->server_send(
        0, R"({"type":"MutationResponse","requestId":0,"success":true,"result":null,)"
           R"("ts":")" + std::string(TS10) + R"(","logLines":[]})");
    c.process_events();
    EXPECT_EQ(future.wait_for(50ms), std::future_status::timeout)
        << "mutation must not resolve before the transition watermark";

    transport->server_send(0, transition_empty(0, TS0, TS10));
    ASSERT_TRUE(pump_until(c, [&] { return future.wait_for(0ms) == std::future_status::ready; }));
    EXPECT_TRUE(future.get().ok());
}

TEST(Client, MutationErrorResolvesImmediately) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));
    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 1));

    auto future = c.mutation("messages:send", {});
    ASSERT_TRUE(transport->wait_for_sent(0, 2));
    transport->server_send(
        0, R"({"type":"MutationResponse","requestId":0,"success":false,)"
           R"("result":"Uncaught ConvexError","errorData":{"code":"TEST"},"logLines":[]})");
    ASSERT_TRUE(pump_until(c, [&] { return future.wait_for(0ms) == std::future_status::ready; }));
    const auto result = future.get();
    ASSERT_TRUE(result.is_app_error());
    EXPECT_EQ(result.app_error()->data.as_object().at("code"), value("TEST"));
}

TEST(Client, ReconnectRebuildsSubscriptionsAndReports) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));

    std::vector<connection_state> states;
    c.on_state_change([&](connection_state s) { states.push_back(s); });

    std::vector<function_result> updates;
    auto sub = c.subscribe("counters:get", {{"name", value("n")}},
                           [&](const function_result& r) { updates.push_back(r); });

    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 2));
    transport->server_send(0, transition_updated(0, TS0, 1, TS5, 0, "1.0"));
    ASSERT_TRUE(pump_until(c, [&] { return updates.size() >= 1; }));

    // Server drops the connection.
    transport->server_close(0, "server going away");
    ASSERT_TRUE(transport->wait_for_attempts(2));
    transport->open(1);
    ASSERT_TRUE(transport->wait_for_sent(1, 2));

    const json connect = json::parse(transport->sent(1)[0]);
    EXPECT_EQ(connect["type"], "Connect");
    EXPECT_EQ(connect["connectionCount"], 1);
    EXPECT_EQ(connect["lastCloseReason"], "server going away");
    EXPECT_EQ(connect["maxObservedTimestamp"], TS5);

    const json mqs = json::parse(transport->sent(1)[1]);
    EXPECT_EQ(mqs["type"], "ModifyQuerySet");
    EXPECT_EQ(mqs["baseVersion"], 0);  // versions reset per connection
    EXPECT_EQ(mqs["newVersion"], 1);
    EXPECT_EQ(mqs["modifications"][0]["queryId"], 0);

    // Old connection object was released.
    EXPECT_TRUE(pump_until(c, [&] { return !transport->connection_alive(0); }, 2s));

    // Fresh transition on the new connection reaches the same subscriber.
    transport->server_send(1, transition_updated(0, TS0, 1, TS6, 0, "2.0"));
    ASSERT_TRUE(pump_until(c, [&] { return updates.size() >= 2; }));
    EXPECT_EQ(updates.back().get_value(), value(2.0));

    // State listener saw the round trip.
    ASSERT_TRUE(pump_until(c, [&] {
        return std::count(states.begin(), states.end(), connection_state::connected) >= 2;
    }));
    EXPECT_TRUE(std::count(states.begin(), states.end(), connection_state::disconnected) >= 1);
}

TEST(Client, InFlightActionFailsOnDisconnect) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));
    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 1));

    auto future = c.action("actions:echoAction", {});
    ASSERT_TRUE(transport->wait_for_sent(0, 2));  // action reached the wire

    transport->server_close(0, "boom");
    ASSERT_TRUE(pump_until(c, [&] { return future.wait_for(0ms) == std::future_status::ready; }));
    const auto result = future.get();
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error_message().find("Connection lost"), std::string::npos);
}

TEST(Client, QueryIsOneShot) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));
    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 1));

    auto future = c.query("counters:get", {{"name", value("n")}});
    ASSERT_TRUE(transport->wait_for_sent(0, 2));  // subscribe went out

    transport->server_send(0, transition_updated(0, TS0, 1, TS5, 0, "7.0"));
    ASSERT_TRUE(pump_until(c, [&] { return future.wait_for(0ms) == std::future_status::ready; }));
    EXPECT_EQ(future.get().get_value(), value(7.0));

    // The one-shot unsubscribes afterwards: a Remove goes out.
    ASSERT_TRUE(transport->wait_for_sent(0, 3));
    const json remove = json::parse(transport->sent(0)[2]);
    EXPECT_EQ(remove["type"], "ModifyQuerySet");
    EXPECT_EQ(remove["modifications"][0]["type"], "Remove");
}

TEST(Client, InactivityTriggersReconnect) {
    auto transport = std::make_shared<mock_transport>();
    auto options = make_options(transport);
    options.server_inactivity_threshold = 100ms;
    client c(std::move(options));

    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 1));
    // No server traffic at all: the client must give up and reconnect.
    EXPECT_TRUE(transport->wait_for_attempts(2, 3s));
}

TEST(Client, ImmediateModeNeedsNoPump) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport, client_options::delivery::immediate));

    std::promise<function_result> got;
    auto once = std::make_shared<std::atomic_bool>(false);
    auto sub = c.subscribe("counters:get", {}, [&got, once](const function_result& r) {
        if (!once->exchange(true)) got.set_value(r);
    });

    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 2));
    transport->server_send(0, transition_updated(0, TS0, 1, TS5, 0, "3.0"));

    auto future = got.get_future();
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get().get_value(), value(3.0));
}

TEST(Client, ShutdownCompletesPendingRequests) {
    auto transport = std::make_shared<mock_transport>();
    std::future<function_result> future;
    {
        client c(make_options(transport));
        ASSERT_TRUE(transport->wait_for_attempts(1));
        transport->open(0);
        future = c.mutation("messages:send", {});
        // Destroy the client with the mutation still in flight.
    }
    ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
    const auto result = future.get();
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error_message().find("shut down"), std::string::npos);
}

TEST(Client, ShutdownDeliversQueuedButUnpumpedCallbacks) {
    // Regression: in pumped mode a completion callback that
    // already moved from `pending` into the event queue was dropped if the
    // client was destroyed before the next process_events(), breaking the
    // future. Shutdown must deliver queued callbacks.
    auto transport = std::make_shared<mock_transport>();
    std::future<function_result> future;
    {
        client c(make_options(transport));
        ASSERT_TRUE(transport->wait_for_attempts(1));
        transport->open(0);
        ASSERT_TRUE(transport->wait_for_sent(0, 1));
        future = c.mutation("messages:send", {});
        ASSERT_TRUE(transport->wait_for_sent(0, 2));
        // Failure responses complete immediately, queueing the callback.
        transport->server_send(0,
                               R"({"type":"MutationResponse","requestId":0,"success":false,)"
                               R"("result":"boom","logLines":[]})");
        // Destroy without ever pumping.
    }
    ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
    const auto result = future.get();  // must not throw broken_promise
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error_message().find("boom"), std::string::npos);
}

TEST(Client, SubscriptionHandleOutlivesClient) {
    auto transport = std::make_shared<mock_transport>();
    client::subscription sub;
    {
        client c(make_options(transport));
        sub = c.subscribe("counters:get", {}, [](const function_result&) {});
    }
    sub.unsubscribe();  // must be a harmless no-op after client destruction
    SUCCEED();
}

TEST(Client, LogLinesReachListenerWithAttribution) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));

    std::vector<log_entry> logs;
    c.on_log_lines([&](const log_entry& e) { logs.push_back(e); });

    auto sub = c.subscribe("messages:list", {}, [](const function_result&) {});
    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 2));

    transport->server_send(
        0, R"({"type":"Transition","startVersion":{"querySet":0,"identity":0,"ts":")" +
               std::string(TS0) + R"("},"endVersion":{"querySet":1,"identity":0,"ts":")" +
               std::string(TS5) +
               R"("},"modifications":[{"type":"QueryUpdated","queryId":0,"value":1.0,)"
               R"("logLines":["[LOG] hello from server"]}]})");
    ASSERT_TRUE(pump_until(c, [&] { return !logs.empty(); }));
    EXPECT_EQ(logs[0].source, log_entry::source_kind::query);
    EXPECT_EQ(logs[0].udf_path, "messages:list");
    ASSERT_EQ(logs[0].lines.size(), 1u);
    EXPECT_EQ(logs[0].lines[0], "[LOG] hello from server");

    // Mutation logs arrive with the response, before the result is released.
    auto future = c.mutation("messages:send", {});
    ASSERT_TRUE(transport->wait_for_sent(0, 3));
    transport->server_send(0,
                           R"({"type":"MutationResponse","requestId":0,"success":true,)"
                           R"("result":null,"ts":")" +
                               std::string(TS10) + R"(","logLines":["[LOG] sending"]})");
    ASSERT_TRUE(pump_until(c, [&] { return logs.size() >= 2; }));
    EXPECT_EQ(logs[1].source, log_entry::source_kind::mutation);
    EXPECT_EQ(logs[1].udf_path, "messages:send");
    EXPECT_EQ(future.wait_for(0ms), std::future_status::timeout);
}

TEST(Client, AuthFailureTerminalWhenRefreshedTokenRejected) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));

    std::vector<std::string> failures;
    c.on_auth_failure([&](std::string reason) { failures.push_back(std::move(reason)); });

    // A fetcher exists, so the first AuthError only forces a refresh+retry.
    c.set_auth(auth_token::user("expiring-jwt"),
               [](bool) { return auth_token::user("refreshed-jwt"); });

    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 2));  // Connect + Authenticate

    transport->server_send(0, R"({"type":"AuthError","error":"expired"})");
    ASSERT_TRUE(transport->wait_for_attempts(2));
    transport->open(1);
    ASSERT_TRUE(transport->wait_for_sent(1, 2));
    const json reauth = json::parse(transport->sent(1)[1]);
    EXPECT_EQ(reauth["type"], "Authenticate");
    EXPECT_EQ(reauth["value"], "refreshed-jwt");
    c.process_events();
    EXPECT_TRUE(failures.empty()) << "first AuthError must not be terminal when a fetcher exists";

    // The refreshed token is rejected too: terminal. The client reports the
    // failure and reconnects unauthenticated.
    transport->server_send(1, R"({"type":"AuthError","error":"still expired",)"
                              R"("baseVersion":0,"authUpdateAttempted":true})");
    ASSERT_TRUE(pump_until(c, [&] { return !failures.empty(); }));
    EXPECT_NE(failures[0].find("still expired"), std::string::npos);

    ASSERT_TRUE(transport->wait_for_attempts(3));
    transport->open(2);
    ASSERT_TRUE(transport->wait_for_sent(2, 1));
    for (const std::string& frame : transport->sent(2)) {
        EXPECT_EQ(json::parse(frame)["type"], "Connect")
            << "no Authenticate may be sent after terminal auth failure";
    }

    // A fresh set_auth gives authentication another chance.
    c.set_auth(auth_token::user("brand-new-jwt"));
    ASSERT_TRUE(transport->wait_for_sent(2, 2));
    EXPECT_EQ(json::parse(transport->sent(2)[1])["type"], "Authenticate");
}

TEST(Client, AuthFailureTerminalImmediatelyWithoutFetcher) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));

    std::vector<std::string> failures;
    c.on_auth_failure([&](std::string reason) { failures.push_back(std::move(reason)); });
    c.set_auth(auth_token::user("static-jwt"));  // no fetcher: cannot ever recover

    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 2));
    transport->server_send(0, R"({"type":"AuthError","error":"bad token"})");
    ASSERT_TRUE(pump_until(c, [&] { return !failures.empty(); }));

    ASSERT_TRUE(transport->wait_for_attempts(2));
    transport->open(1);
    ASSERT_TRUE(transport->wait_for_sent(1, 1));
    for (const std::string& frame : transport->sent(1)) {
        EXPECT_EQ(json::parse(frame)["type"], "Connect");
    }
}

TEST(Client, InfoSnapshotTracksConnectionAndInflight) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));

    ASSERT_TRUE(transport->wait_for_attempts(1));
    transport->open(0);
    ASSERT_TRUE(transport->wait_for_sent(0, 1));

    connection_info i = c.info();
    EXPECT_EQ(i.state, connection_state::connected);
    EXPECT_EQ(i.last_close_reason, "InitialConnect");
    EXPECT_EQ(i.connection_count, 1u);
    EXPECT_EQ(i.inflight_mutations, 0u);

    auto future = c.mutation("messages:send", {});
    ASSERT_TRUE(transport->wait_for_sent(0, 2));
    i = c.info();
    EXPECT_EQ(i.inflight_mutations, 1u);
    EXPECT_EQ(i.inflight_actions, 0u);

    transport->server_close(0, "server going away");
    ASSERT_TRUE(transport->wait_for_attempts(2));
    i = c.info();
    EXPECT_EQ(i.last_close_reason, "server going away");
    EXPECT_GE(i.retries, 1u);
    EXPECT_EQ(i.inflight_mutations, 1u) << "mutations survive reconnects";
}

TEST(WsUrl, Derivation) {
    EXPECT_EQ(deployment_to_ws_url("https://happy-animal-123.convex.cloud"),
              "wss://happy-animal-123.convex.cloud/api/sync");
    EXPECT_EQ(deployment_to_ws_url("http://127.0.0.1:3210"), "ws://127.0.0.1:3210/api/sync");
    EXPECT_EQ(deployment_to_ws_url("https://x.convex.cloud/"), "wss://x.convex.cloud/api/sync");
    EXPECT_THROW(deployment_to_ws_url("ftp://x"), std::invalid_argument);
    EXPECT_THROW(deployment_to_ws_url("no-scheme"), std::invalid_argument);
}
