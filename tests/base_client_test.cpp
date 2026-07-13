// Ports of the convex-rs BaseConvexClient state-machine tests: query set
// versioning, subscription dedup/refcounting, mutation ordering (results
// gated on the transition watermark), action semantics, and reconnect
// rebuild rules.

#include <convex/base_client.h>
#include <gtest/gtest.h>

using namespace convex;

namespace {

transition_message make_transition(state_version start, state_version end,
                                   std::vector<state_modification> mods = {}) {
    transition_message t;
    t.start_version = start;
    t.end_version = end;
    t.modifications = std::move(mods);
    return t;
}

query_updated updated(query_id id, value v) {
    query_updated u;
    u.id = id;
    u.result = std::move(v);
    return u;
}

// Drains the outgoing queue, asserting the sequence of message alternatives.
template <typename T>
T expect_message(base_client& c) {
    auto m = c.pop_next_message();
    if (!m.has_value()) throw std::runtime_error("expected a message, queue empty");
    return std::get<T>(std::move(*m));
}

void expect_empty(base_client& c) { EXPECT_FALSE(c.pop_next_message().has_value()); }

}  // namespace

// ------------------------------------------------------------- query set

TEST(Subscriptions, AddEmitsModifyQuerySet) {
    base_client c;
    const auto sub = c.subscribe("messages:list", {{"channel", value("general")}});

    const auto m = expect_message<modify_query_set_message>(c);
    EXPECT_EQ(m.base_version, 0u);
    EXPECT_EQ(m.new_version, 1u);
    ASSERT_EQ(m.modifications.size(), 1u);
    const auto& add = std::get<query_add>(m.modifications[0]);
    EXPECT_EQ(add.id, sub.query);
    EXPECT_EQ(add.udf_path, "messages:list");
    EXPECT_EQ(add.args_json, R"([{"channel":"general"}])");
    expect_empty(c);
}

TEST(Subscriptions, IdenticalQueriesShareOneSubscription) {
    base_client c;
    const auto a = c.subscribe("messages:list", {{"channel", value("general")}});
    (void)expect_message<modify_query_set_message>(c);

    const auto b = c.subscribe("messages:list", {{"channel", value("general")}});
    EXPECT_EQ(a.query, b.query);
    EXPECT_NE(a.index, b.index);
    expect_empty(c);  // no wire traffic for the second subscriber

    // Different args are a different query.
    const auto other = c.subscribe("messages:list", {{"channel", value("random")}});
    EXPECT_NE(other.query, a.query);
    (void)expect_message<modify_query_set_message>(c);
}

TEST(Subscriptions, RemoveOnlyWhenLastSubscriberDrops) {
    base_client c;
    const auto a = c.subscribe("messages:list", {});
    const auto b = c.subscribe("messages:list", {});
    (void)expect_message<modify_query_set_message>(c);

    c.unsubscribe(a);
    expect_empty(c);

    c.unsubscribe(b);
    const auto m = expect_message<modify_query_set_message>(c);
    EXPECT_EQ(m.base_version, 1u);
    EXPECT_EQ(m.new_version, 2u);
    ASSERT_EQ(m.modifications.size(), 1u);
    EXPECT_EQ(std::get<query_remove>(m.modifications[0]).id, a.query);

    // Double-unsubscribe is a no-op.
    c.unsubscribe(b);
    expect_empty(c);
}

TEST(Subscriptions, SubscriberIndicesNeverReused) {
    base_client c;
    const auto a = c.subscribe("q:one", {});
    c.unsubscribe(a);
    const auto b = c.subscribe("q:one", {});
    EXPECT_GT(b.index, a.index);
}

// -------------------------------------------------------------- mutations

TEST(Mutations, ResultGatedOnTransitionWatermark) {
    base_client c;
    const request_id rid = c.mutation("messages:send", {{"body", value("hi")}});
    (void)expect_message<mutation_request_message>(c);

    // Server responds success at ts=10; the caller must NOT learn about it
    // yet — query results at ts>=10 haven't arrived.
    mutation_response_message resp;
    resp.id = rid;
    resp.result = function_result::success(value(nullptr));
    resp.ts = timestamp{10};
    auto r1 = c.receive_message(resp);
    EXPECT_TRUE(r1.completed_requests.empty());

    // A transition to ts=9 is not enough.
    auto r2 = c.receive_message(make_transition({0, 0, 0}, {0, 0, 9}));
    EXPECT_TRUE(r2.completed_requests.empty());

    // ts=10 releases it.
    auto r3 = c.receive_message(make_transition({0, 0, 9}, {0, 0, 10}));
    ASSERT_EQ(r3.completed_requests.size(), 1u);
    EXPECT_EQ(r3.completed_requests[0].first, rid);
    EXPECT_TRUE(r3.completed_requests[0].second.ok());
}

TEST(Mutations, ResultImmediateWhenWatermarkAlreadyPassed) {
    base_client c;
    // Client already saw ts=20.
    (void)c.receive_message(make_transition({0, 0, 0}, {0, 0, 20}));

    const request_id rid = c.mutation("messages:send", {});
    (void)expect_message<mutation_request_message>(c);
    mutation_response_message resp;
    resp.id = rid;
    resp.result = function_result::success(value(1.0));
    resp.ts = timestamp{15};  // <= 20
    auto r = c.receive_message(resp);
    ASSERT_EQ(r.completed_requests.size(), 1u);
    EXPECT_EQ(r.completed_requests[0].first, rid);
}

TEST(Mutations, ErrorsCompleteImmediately) {
    base_client c;
    const request_id rid = c.mutation("messages:send", {});
    (void)expect_message<mutation_request_message>(c);

    mutation_response_message resp;
    resp.id = rid;
    resp.result = function_result::error(convex_error{"boom", value(value_object{
                                             {"code", value("TEST")}})});
    auto r = c.receive_message(resp);
    ASSERT_EQ(r.completed_requests.size(), 1u);
    EXPECT_TRUE(r.completed_requests[0].second.is_app_error());
    EXPECT_EQ(r.completed_requests[0].second.app_error()->data.as_object().at("code"),
              value("TEST"));
}

TEST(Actions, CompleteImmediately) {
    base_client c;
    const request_id rid = c.action("actions:echoAction", {});
    (void)expect_message<action_request_message>(c);

    action_response_message resp;
    resp.id = rid;
    resp.result = function_result::success(value("done"));
    auto r = c.receive_message(resp);
    ASSERT_EQ(r.completed_requests.size(), 1u);
    EXPECT_EQ(r.completed_requests[0].second.get_value(), value("done"));
}

// ------------------------------------------------------------ transitions

TEST(Transitions, ApplyAndExposeResults) {
    base_client c;
    const auto sub = c.subscribe("counters:get", {{"name", value("n")}});
    (void)expect_message<modify_query_set_message>(c);

    auto r = c.receive_message(make_transition(
        {0, 0, 0}, {1, 0, 5}, {updated(sub.query, value(std::int64_t{7}))}));
    EXPECT_TRUE(r.state_changed);
    ASSERT_NE(c.latest_result(sub.query), nullptr);
    EXPECT_EQ(c.latest_result(sub.query)->get_value(), value(std::int64_t{7}));
    EXPECT_EQ(c.max_observed_timestamp(), timestamp{5});
}

TEST(Transitions, QueryFailureCarriesConvexError) {
    base_client c;
    const auto sub = c.subscribe("errors:throwConvexError", {});
    (void)expect_message<modify_query_set_message>(c);

    query_failed f;
    f.id = sub.query;
    f.error_message = "Uncaught ConvexError";
    f.error_data = value(value_object{{"code", value("TEST")}});
    auto r = c.receive_message(make_transition({0, 0, 0}, {1, 0, 3}, {f}));
    EXPECT_TRUE(r.state_changed);
    const function_result* res = c.latest_result(sub.query);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(res->is_app_error());
}

TEST(Transitions, StartVersionMismatchForcesReconnect) {
    base_client c;
    auto r = c.receive_message(make_transition({3, 0, 7}, {4, 0, 8}));
    ASSERT_TRUE(r.reconnect_reason.has_value());
    EXPECT_EQ(*r.reconnect_reason, "StartVersionMismatch");
}

TEST(Errors, AuthAndFatalForceReconnect) {
    base_client c;
    EXPECT_TRUE(c.receive_message(auth_error_message{"bad", std::nullopt, false})
                    .reconnect_reason.has_value());
    EXPECT_TRUE(c.receive_message(fatal_error_message{"dead"}).reconnect_reason.has_value());
    EXPECT_FALSE(c.receive_message(ping_message{}).reconnect_reason.has_value());
}

TEST(Errors, UnassembledTransitionChunkForcesReconnect) {
    // Chunks are reassembled by the transport layer; the state machine treats
    // a raw chunk as a protocol violation rather than corrupting query state.
    base_client c;
    auto r = c.receive_message(transition_chunk_message{"{}", 0, 1, "2"});
    ASSERT_TRUE(r.reconnect_reason.has_value());
    EXPECT_EQ(*r.reconnect_reason, "ProtocolError: unassembled TransitionChunk");
}

TEST(Errors, AuthErrorCarriesUpdateAttemptedFlag) {
    base_client c;
    auto plain = c.receive_message(auth_error_message{"bad", std::nullopt, false});
    EXPECT_TRUE(plain.auth_error);
    EXPECT_FALSE(plain.auth_update_attempted);

    auto attempted = c.receive_message(auth_error_message{"still bad", identity_version{1}, true});
    EXPECT_TRUE(attempted.auth_error);
    EXPECT_TRUE(attempted.auth_update_attempted);

    auto fatal = c.receive_message(fatal_error_message{"dead"});
    EXPECT_FALSE(fatal.auth_error);
}

// -------------------------------------------------------------- log lines

TEST(LogLines, QueryLogsAttributedToUdfPathAndNotReplayed) {
    base_client c;
    const auto sub = c.subscribe("messages:list", {{"channel", value("general")}});
    (void)expect_message<modify_query_set_message>(c);

    query_updated u = updated(sub.query, value(1.0));
    u.log_lines = {"[LOG] first", "[LOG] second"};
    auto r = c.receive_message(make_transition({0, 0, 0}, {1, 0, 5}, {u}));
    ASSERT_EQ(r.log_entries.size(), 1u);
    EXPECT_EQ(r.log_entries[0].source, log_entry::source_kind::query);
    EXPECT_EQ(r.log_entries[0].udf_path, "messages:list");
    EXPECT_EQ(r.log_entries[0].lines, (std::vector<std::string>{"[LOG] first", "[LOG] second"}));

    // Log lines are per-execution events: an update without them emits none,
    // and nothing from the previous update is replayed.
    auto r2 = c.receive_message(
        make_transition({1, 0, 5}, {1, 0, 6}, {updated(sub.query, value(2.0))}));
    EXPECT_TRUE(r2.log_entries.empty());
}

TEST(LogLines, QueryFailureLogsAttributed) {
    base_client c;
    const auto sub = c.subscribe("errors:throwConvexError", {});
    (void)expect_message<modify_query_set_message>(c);

    query_failed f;
    f.id = sub.query;
    f.error_message = "boom";
    f.log_lines = {"[ERROR] about to throw"};
    auto r = c.receive_message(make_transition({0, 0, 0}, {1, 0, 3}, {f}));
    ASSERT_EQ(r.log_entries.size(), 1u);
    EXPECT_EQ(r.log_entries[0].source, log_entry::source_kind::query);
    EXPECT_EQ(r.log_entries[0].udf_path, "errors:throwConvexError");
}

TEST(LogLines, MutationLogsDeliveredEvenWhileResultIsGated) {
    base_client c;
    const request_id rid = c.mutation("messages:send", {{"body", value("hi")}});
    (void)expect_message<mutation_request_message>(c);

    mutation_response_message resp;
    resp.id = rid;
    resp.result = function_result::success(value(nullptr));
    resp.ts = timestamp{10};
    resp.log_lines = {"[LOG] sent"};
    auto r = c.receive_message(resp);
    // The result is held for the watermark, but the log lines arrive now.
    EXPECT_TRUE(r.completed_requests.empty());
    ASSERT_EQ(r.log_entries.size(), 1u);
    EXPECT_EQ(r.log_entries[0].source, log_entry::source_kind::mutation);
    EXPECT_EQ(r.log_entries[0].udf_path, "messages:send");
}

TEST(LogLines, ActionLogsAttributed) {
    base_client c;
    const request_id rid = c.action("actions:echoAction", {});
    (void)expect_message<action_request_message>(c);

    action_response_message resp;
    resp.id = rid;
    resp.result = function_result::success(value("done"));
    resp.log_lines = {"[LOG] echoed"};
    auto r = c.receive_message(resp);
    ASSERT_EQ(r.log_entries.size(), 1u);
    EXPECT_EQ(r.log_entries[0].source, log_entry::source_kind::action);
    EXPECT_EQ(r.log_entries[0].udf_path, "actions:echoAction");
}

// ---------------------------------------------------------- introspection

TEST(Introspection, InflightCountsTrackRequestLifecycles) {
    base_client c;
    EXPECT_EQ(c.inflight_mutations(), 0u);
    EXPECT_EQ(c.inflight_actions(), 0u);

    const request_id mut = c.mutation("messages:send", {});
    const request_id act = c.action("actions:echoAction", {});
    EXPECT_EQ(c.inflight_mutations(), 1u);
    EXPECT_EQ(c.inflight_actions(), 1u);

    action_response_message aresp;
    aresp.id = act;
    aresp.result = function_result::success(value(nullptr));
    (void)c.receive_message(aresp);
    EXPECT_EQ(c.inflight_actions(), 0u);

    // A gated mutation still counts as in flight until it is delivered.
    mutation_response_message mresp;
    mresp.id = mut;
    mresp.result = function_result::success(value(nullptr));
    mresp.ts = timestamp{10};
    (void)c.receive_message(mresp);
    EXPECT_EQ(c.inflight_mutations(), 1u);
    (void)c.receive_message(make_transition({0, 0, 0}, {0, 0, 10}));
    EXPECT_EQ(c.inflight_mutations(), 0u);
}

// ------------------------------------------------------------------- auth

TEST(Auth, VersionsIncrementPerAuthenticate) {
    base_client c;
    c.set_auth(auth_token::user("jwt-1"));
    auto m1 = expect_message<authenticate_message>(c);
    EXPECT_EQ(m1.base_version, 0u);

    c.set_auth(auth_token::user("jwt-2"));
    auto m2 = expect_message<authenticate_message>(c);
    EXPECT_EQ(m2.base_version, 1u);

    c.set_auth(auth_token::none());
    auto m3 = expect_message<authenticate_message>(c);
    EXPECT_EQ(m3.base_version, 2u);
    EXPECT_EQ(m3.token.type, auth_token::kind::none);
}

// ---------------------------------------------------------------- restart

TEST(Restart, RebuildsAuthQueriesAndMutationsInOrder) {
    base_client c;
    c.set_auth(auth_token::user("jwt"));
    const auto sub = c.subscribe("messages:list", {{"channel", value("general")}});
    const request_id mut1 = c.mutation("messages:send", {{"body", value("one")}});
    const request_id mut2 = c.mutation("messages:send", {{"body", value("two")}});
    const request_id act = c.action("actions:echoAction", {});
    // Give the first query a journal so restart must carry it.
    query_updated u = updated(sub.query, value(1.0));
    u.journal = "journal-token";
    (void)c.receive_message(make_transition({0, 0, 0}, {1, 1, 5}, {u}));

    // Simulate the runtime having sent everything queued so far: only
    // actions already on the wire are at risk and must fail on restart.
    while (c.pop_next_message().has_value()) {
    }

    const auto failed = c.restart(auth_token::user("fresh-jwt"));

    // In-flight action fails; mutations survive.
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_EQ(failed[0].first, act);
    EXPECT_FALSE(failed[0].second.ok());

    // Order: Authenticate (fresh token, baseVersion 0), one ModifyQuerySet
    // 0->1 with the journal, then mutations in original order.
    auto auth = expect_message<authenticate_message>(c);
    EXPECT_EQ(auth.base_version, 0u);
    EXPECT_EQ(auth.token.value, "fresh-jwt");

    auto qs = expect_message<modify_query_set_message>(c);
    EXPECT_EQ(qs.base_version, 0u);
    EXPECT_EQ(qs.new_version, 1u);
    ASSERT_EQ(qs.modifications.size(), 1u);
    const auto& add = std::get<query_add>(qs.modifications[0]);
    EXPECT_EQ(add.id, sub.query);
    ASSERT_TRUE(add.journal.has_value());
    EXPECT_EQ(*add.journal, "journal-token");

    EXPECT_EQ(expect_message<mutation_request_message>(c).id, mut1);
    EXPECT_EQ(expect_message<mutation_request_message>(c).id, mut2);
    expect_empty(c);

    // Old results were wiped; the restarted query set is outstanding.
    EXPECT_EQ(c.latest_result(sub.query), nullptr);
    EXPECT_FALSE(c.has_synced_past_last_restart());
    (void)c.receive_message(make_transition({0, 0, 0}, {1, 1, 6}, {updated(sub.query, value(2.0))}));
    EXPECT_TRUE(c.has_synced_past_last_restart());
}

TEST(Restart, ClearsStaleQueuedMessages) {
    // Regression parallel to convex-rs "reconnect does not send duplicate
    // version messages": anything still queued at restart carries stale
    // versions and must be dropped.
    base_client c;
    (void)c.subscribe("a:b", {});
    (void)c.subscribe("c:d", {});  // two queued ModifyQuerySets (0->1, 1->2)

    (void)c.restart();
    const auto qs = expect_message<modify_query_set_message>(c);
    EXPECT_EQ(qs.base_version, 0u);
    EXPECT_EQ(qs.new_version, 1u);
    EXPECT_EQ(qs.modifications.size(), 2u);
    expect_empty(c);
}

TEST(Restart, ConnectMessageTracksCountAndTimestamp) {
    base_client c;
    const auto first = c.make_connect_message("InitialConnect");
    EXPECT_EQ(first.connection_count, 0u);
    EXPECT_EQ(first.session_id, c.session_id());
    EXPECT_FALSE(first.max_observed_timestamp.has_value());

    (void)c.receive_message(make_transition({0, 0, 0}, {0, 0, 42}));
    const auto second = c.make_connect_message("InactiveServer");
    EXPECT_EQ(second.connection_count, 1u);
    EXPECT_EQ(second.last_close_reason, "InactiveServer");
    EXPECT_EQ(second.max_observed_timestamp, timestamp{42});
}

TEST(Restart, NeverSentActionsAreResentNotFailed) {
    base_client c;
    // The action is enqueued but the transport never drained it, so it was
    // never on the wire: resending is safe and expected.
    const request_id act = c.action("actions:echoAction", {});

    const auto failed = c.restart();
    EXPECT_TRUE(failed.empty());
    EXPECT_EQ(expect_message<action_request_message>(c).id, act);
    expect_empty(c);
}

TEST(Restart, CompletedButUndeliveredMutationsAreResent) {
    base_client c;
    const request_id rid = c.mutation("messages:send", {});
    (void)expect_message<mutation_request_message>(c);

    mutation_response_message resp;
    resp.id = rid;
    resp.result = function_result::success(value(nullptr));
    resp.ts = timestamp{100};
    (void)c.receive_message(resp);  // completed, but watermark never reached

    (void)c.restart();
    EXPECT_EQ(expect_message<mutation_request_message>(c).id, rid);
}
