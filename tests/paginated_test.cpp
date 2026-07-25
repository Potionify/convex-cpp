// paginated_query tests against the mock transport: paginationOpts wire
// shape, cursor chaining, combined results across live page subscriptions,
// status transitions, args-change / InvalidCursor resets, page splitting,
// and journal resend on reconnect (the seam-stability guarantee).

#include <chrono>
#include <thread>
#include <vector>

#include <convex/paginated.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "mock_transport.h"

using namespace convex;
using convex::testing::mock_transport;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

client_options make_options(std::shared_ptr<mock_transport> transport) {
    client_options o;
    o.deployment_url = "https://unit-test.convex.cloud";
    o.websocket = std::move(transport);
    o.initial_backoff = 10ms;  // keep reconnect tests fast
    o.max_backoff = 50ms;
    o.server_inactivity_threshold = 60s;
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

// Base64 little-endian encodings of small timestamps.
constexpr const char* TS0 = "AAAAAAAAAAA=";
constexpr const char* TS1 = "AQAAAAAAAAA=";
constexpr const char* TS2 = "AgAAAAAAAAA=";
constexpr const char* TS3 = "AwAAAAAAAAA=";
constexpr const char* TS4 = "BAAAAAAAAAA=";

// A PaginationResult value: string items for easy comparison.
json page_value(const std::vector<std::string>& items, bool is_done, const std::string& cursor,
                const char* page_status = nullptr, const char* split_cursor = nullptr) {
    json v{{"page", items}, {"isDone", is_done}, {"continueCursor", cursor}};
    if (page_status) v["pageStatus"] = page_status;
    if (split_cursor) v["splitCursor"] = split_cursor;
    return v;
}

struct version {
    int query_set;
    const char* ts;
};

json transition_shell(version start, version end) {
    return json{{"type", "Transition"},
                {"startVersion", {{"querySet", start.query_set}, {"identity", 0}, {"ts", start.ts}}},
                {"endVersion", {{"querySet", end.query_set}, {"identity", 0}, {"ts", end.ts}}},
                {"modifications", json::array()}};
}

std::string transition_page(version start, version end, int query, const json& value,
                            const char* journal = nullptr) {
    json t = transition_shell(start, end);
    json mod{{"type", "QueryUpdated"}, {"queryId", query}, {"value", value},
             {"logLines", json::array()}};
    if (journal) mod["journal"] = journal;
    t["modifications"].push_back(std::move(mod));
    return t.dump();
}

std::string transition_failed(version start, version end, int query, const std::string& message) {
    json t = transition_shell(start, end);
    t["modifications"].push_back(json{{"type", "QueryFailed"},
                                      {"queryId", query},
                                      {"errorMessage", message},
                                      {"logLines", json::array()}});
    return t.dump();
}

std::vector<value> items(std::initializer_list<const char*> strings) {
    std::vector<value> out;
    for (const char* s : strings) out.emplace_back(s);
    return out;
}

// A fixture-less harness: client + helper + recorded snapshots, connected
// with the first page's ModifyQuerySet already on the wire.
struct harness {
    std::shared_ptr<mock_transport> transport = std::make_shared<mock_transport>();
    client c{make_options(transport)};
    std::vector<paginated_snapshot> snapshots;
    paginated_query pq;

    explicit harness(std::size_t initial_num_items = 2,
                     value_object args = {{"channel", value("general")}})
        : pq(c,
             paginated_query::options{"messages:listPaginated", std::move(args),
                                      initial_num_items},
             [this](const paginated_snapshot& s) { snapshots.push_back(s); }) {
        EXPECT_TRUE(transport->wait_for_attempts(1));
        transport->open(0);
        EXPECT_TRUE(transport->wait_for_sent(0, 2));  // Connect + ModifyQuerySet
    }

    json sent_json(std::size_t attempt, std::size_t index) {
        return json::parse(transport->sent(attempt).at(index));
    }
    // The Add modification for `query_id`, from anywhere in the sent stream
    // (a split adds two pages, so positions are no longer predictable).
    json add_for(int query_id, std::size_t attempt = 0) {
        for (const std::string& raw : transport->sent(attempt)) {
            const json j = json::parse(raw);
            if (j.value("type", "") != "ModifyQuerySet") continue;
            for (const json& mod : j["modifications"]) {
                if (mod["type"] == "Add" && mod["queryId"] == query_id) return mod;
            }
        }
        return json::object();
    }
    bool sent_remove(int query_id, std::size_t attempt = 0) {
        for (const std::string& raw : transport->sent(attempt)) {
            const json j = json::parse(raw);
            if (j.value("type", "") != "ModifyQuerySet") continue;
            for (const json& mod : j["modifications"]) {
                if (mod["type"] == "Remove" && mod["queryId"] == query_id) return true;
            }
        }
        return false;
    }
    // A copy of the paginationOpts a page was subscribed with. Returned by
    // value: a reference into add_for's temporary would dangle.
    json opts_for(int query_id, std::size_t attempt = 0) {
        const json add = add_for(query_id, attempt);
        return add.contains("args") ? add["args"][0]["paginationOpts"] : json::object();
    }
    double pagination_id(int query_id) { return opts_for(query_id)["id"].get<double>(); }
    bool wait_status(pagination_status s) {
        return pump_until(c, [&] { return pq.snapshot().status == s; });
    }
    // Pump with nothing to wait for, so anything pending lands before an
    // assertion that something did *not* happen.
    void settle() { pump_until(c, [] { return false; }, 50ms); }
};

}  // namespace

TEST(Paginated, FirstPageWireShapeAndResult) {
    harness h;
    EXPECT_EQ(h.pq.snapshot().status, pagination_status::loading_first_page);
    EXPECT_TRUE(h.pq.snapshot().is_loading());

    const json mqs = h.sent_json(0, 1);
    ASSERT_EQ(mqs["type"], "ModifyQuerySet");
    const json& add = mqs["modifications"][0];
    EXPECT_EQ(add["type"], "Add");
    EXPECT_EQ(add["queryId"], 0);
    EXPECT_EQ(add["udfPath"], "messages:listPaginated");
    const json& args = add["args"][0];
    EXPECT_EQ(args["channel"], "general");
    const json& opts = args["paginationOpts"];
    // numItems and id must be plain JSON numbers (float64): the server-side
    // paginationOptsValidator uses v.number() and rejects $integer.
    ASSERT_TRUE(opts["numItems"].is_number());
    EXPECT_EQ(opts["numItems"].get<double>(), 2.0);
    EXPECT_TRUE(opts["cursor"].is_null());
    ASSERT_TRUE(opts["id"].is_number());
    EXPECT_EQ(opts.size(), 3u) << "no endCursor/limits unless requested";

    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    const auto snap = h.pq.snapshot();
    EXPECT_EQ(snap.results, items({"a", "b"}));
    EXPECT_FALSE(snap.is_loading());
    ASSERT_FALSE(h.snapshots.empty());
    EXPECT_EQ(h.snapshots.back().results, snap.results);
    EXPECT_EQ(h.snapshots.back().status, snap.status);
}

TEST(Paginated, LoadMoreChainsCursorAndCombinesPages) {
    harness h;
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    const double id0 = h.sent_json(0, 1)["modifications"][0]["args"][0]["paginationOpts"]["id"]
                           .get<double>();

    EXPECT_TRUE(h.pq.load_more(3));
    EXPECT_EQ(h.pq.snapshot().status, pagination_status::loading_more);
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b"})) << "loaded pages stay visible";
    ASSERT_TRUE(h.transport->wait_for_sent(0, 3));
    const json mqs = h.sent_json(0, 2);
    const json& add = mqs["modifications"][0];
    EXPECT_EQ(add["queryId"], 1);
    const json& opts = add["args"][0]["paginationOpts"];
    EXPECT_EQ(opts["cursor"], "c1") << "next page starts at the previous continueCursor";
    EXPECT_EQ(opts["numItems"].get<double>(), 3.0);
    EXPECT_EQ(opts["id"].get<double>(), id0) << "one pagination session, one id";

    h.transport->server_send(
        0, transition_page({1, TS1}, {2, TS2}, 1, page_value({"c"}, true, "c2"), "j1"));
    ASSERT_TRUE(h.wait_status(pagination_status::exhausted));
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b", "c"}));

    // Exhausted: load_more is a no-op.
    EXPECT_FALSE(h.pq.load_more(3));
    EXPECT_EQ(h.transport->sent(0).size(), 3u);

    // The observed status sequence, via the callback.
    std::vector<pagination_status> seen;
    for (const auto& s : h.snapshots) seen.push_back(s.status);
    EXPECT_EQ(seen, (std::vector<pagination_status>{pagination_status::can_load_more,
                                                    pagination_status::loading_more,
                                                    pagination_status::exhausted}));
}

TEST(Paginated, LoadMoreWhileLoadingIsNoop) {
    harness h;
    EXPECT_FALSE(h.pq.load_more(3)) << "first page still loading";
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));

    EXPECT_TRUE(h.pq.load_more(3));
    EXPECT_FALSE(h.pq.load_more(3)) << "second call while the page is in flight";
    ASSERT_TRUE(h.transport->wait_for_sent(0, 3));
    EXPECT_EQ(h.transport->sent(0).size(), 3u) << "exactly one page subscription added";
}

TEST(Paginated, NonLastPageUpdatePropagatesIntoCombinedResults) {
    harness h;
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    ASSERT_TRUE(h.pq.load_more(2));
    h.transport->server_send(
        0, transition_page({1, TS1}, {2, TS2}, 1, page_value({"c"}, true, "c2"), "j1"));
    ASSERT_TRUE(h.wait_status(pagination_status::exhausted));

    // The first page grows an item (live update); the seam to page 2 holds.
    h.transport->server_send(0, transition_page({2, TS2}, {2, TS3}, 0,
                                                page_value({"a", "a2", "b"}, false, "c1"), "j0b"));
    ASSERT_TRUE(pump_until(h.c, [&] { return h.pq.snapshot().results.size() == 4; }));
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "a2", "b", "c"}));
    EXPECT_EQ(h.pq.snapshot().status, pagination_status::exhausted)
        << "status tracks the last page, not the updated one";
}

TEST(Paginated, SetArgsResetsToFreshFirstPage) {
    harness h;
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    const double id0 = h.sent_json(0, 1)["modifications"][0]["args"][0]["paginationOpts"]["id"]
                           .get<double>();

    EXPECT_FALSE(h.pq.set_args({{"channel", value("general")}})) << "identical args: no reset";
    EXPECT_EQ(h.transport->sent(0).size(), 2u);

    EXPECT_TRUE(h.pq.set_args({{"channel", value("random")}}));
    EXPECT_EQ(h.pq.snapshot().status, pagination_status::loading_first_page);
    EXPECT_TRUE(h.pq.snapshot().results.empty());

    // Wire: Remove(q0), then Add(q1) with the new args and a fresh session id.
    ASSERT_TRUE(h.transport->wait_for_sent(0, 4));
    EXPECT_EQ(h.sent_json(0, 2)["modifications"][0]["type"], "Remove");
    const json add_mqs = h.sent_json(0, 3);
    const json& add = add_mqs["modifications"][0];
    EXPECT_EQ(add["type"], "Add");
    EXPECT_EQ(add["queryId"], 1);
    EXPECT_EQ(add["args"][0]["channel"], "random");
    const json& opts = add["args"][0]["paginationOpts"];
    EXPECT_TRUE(opts["cursor"].is_null());
    EXPECT_EQ(opts["numItems"].get<double>(), 2.0) << "reset uses initial_num_items";
    EXPECT_NE(opts["id"].get<double>(), id0) << "a reset starts a new pagination session";
}

TEST(Paginated, ReconnectResendsEveryPageWithItsJournal) {
    harness h;
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    ASSERT_TRUE(h.pq.load_more(2));
    h.transport->server_send(
        0, transition_page({1, TS1}, {2, TS2}, 1, page_value({"c"}, false, "c2"), "j1"));
    ASSERT_TRUE(pump_until(h.c, [&] { return h.pq.snapshot().results.size() == 3; }));

    h.transport->server_close(0, "SimulatedNetworkBlip");
    ASSERT_TRUE(h.transport->wait_for_attempts(2));
    h.transport->open(1);
    ASSERT_TRUE(h.transport->wait_for_sent(1, 2));

    // Loaded items survive the disconnect (stale-but-visible, like convex-js).
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b", "c"}));

    // One rebuild ModifyQuerySet restoring both pages, each with the journal
    // the server issued — this is what keeps page boundaries seam-free.
    const json mqs = h.sent_json(1, 1);
    ASSERT_EQ(mqs["type"], "ModifyQuerySet");
    EXPECT_EQ(mqs["baseVersion"], 0);
    EXPECT_EQ(mqs["newVersion"], 1);
    ASSERT_EQ(mqs["modifications"].size(), 2u);
    bool saw_q0 = false, saw_q1 = false;
    for (const json& mod : mqs["modifications"]) {
        EXPECT_EQ(mod["type"], "Add");
        if (mod["queryId"] == 0) {
            saw_q0 = true;
            EXPECT_EQ(mod["journal"], "j0");
            EXPECT_TRUE(mod["args"][0]["paginationOpts"]["cursor"].is_null());
        } else if (mod["queryId"] == 1) {
            saw_q1 = true;
            EXPECT_EQ(mod["journal"], "j1");
            EXPECT_EQ(mod["args"][0]["paginationOpts"]["cursor"], "c1");
        }
    }
    EXPECT_TRUE(saw_q0 && saw_q1);

    // Fresh results on the new connection flow into the same pages.
    const json both = [] {
        json t = transition_shell({0, TS0}, {1, TS3});
        t["modifications"].push_back(json{{"type", "QueryUpdated"}, {"queryId", 0},
                                          {"value", page_value({"a", "b"}, false, "c1")},
                                          {"logLines", json::array()}, {"journal", "j0"}});
        t["modifications"].push_back(json{{"type", "QueryUpdated"}, {"queryId", 1},
                                          {"value", page_value({"c", "d"}, false, "c2")},
                                          {"logLines", json::array()}, {"journal", "j1"}});
        return t;
    }();
    h.transport->server_send(1, both.dump());
    ASSERT_TRUE(pump_until(h.c, [&] { return h.pq.snapshot().results.size() == 4; }));
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b", "c", "d"}));
}

TEST(Paginated, InvalidCursorResetsPagination) {
    harness h;
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    ASSERT_TRUE(h.pq.load_more(2));
    const double id0 = h.sent_json(0, 1)["modifications"][0]["args"][0]["paginationOpts"]["id"]
                           .get<double>();

    h.transport->server_send(
        0, transition_failed({1, TS1}, {2, TS2}, 1,
                             "InvalidCursor: Tried to run a query starting from a cursor "
                             "created by a different query"));
    ASSERT_TRUE(h.wait_status(pagination_status::loading_first_page));
    EXPECT_TRUE(h.pq.snapshot().results.empty());

    // Remove(q0), Remove(q1), Add(q2) with a fresh id from cursor null.
    ASSERT_TRUE(h.transport->wait_for_sent(0, 6));
    const json add_mqs = h.sent_json(0, 5);
    const json& add = add_mqs["modifications"][0];
    EXPECT_EQ(add["type"], "Add");
    EXPECT_EQ(add["queryId"], 2);
    EXPECT_TRUE(add["args"][0]["paginationOpts"]["cursor"].is_null());
    EXPECT_NE(add["args"][0]["paginationOpts"]["id"].get<double>(), id0);

    h.transport->server_send(
        0, transition_page({2, TS2}, {5, TS3}, 2, page_value({"x", "y"}, false, "d1"), "k0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    EXPECT_EQ(h.pq.snapshot().results, items({"x", "y"}));
}

TEST(Paginated, OtherErrorsSurfaceWithoutReset) {
    harness h;
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    ASSERT_TRUE(h.pq.load_more(2));

    h.transport->server_send(
        0, transition_failed({1, TS1}, {2, TS2}, 1, "Server Error: Uncaught Error: boom"));
    ASSERT_TRUE(h.wait_status(pagination_status::error));
    const auto snap = h.pq.snapshot();
    EXPECT_EQ(snap.results, items({"a", "b"})) << "pages before the failure stay visible";
    ASSERT_TRUE(snap.error.has_value());
    EXPECT_NE(snap.error->error_message().find("boom"), std::string::npos);
    EXPECT_FALSE(snap.is_loading());

    EXPECT_FALSE(h.pq.load_more(2)) << "no loading past an error";
    EXPECT_EQ(h.transport->sent(0).size(), 3u) << "no reset traffic for ordinary errors";
}

TEST(Paginated, SplitRequiredSplitsPageInTwo) {
    harness h;
    // A page that outgrew the read limits: incomplete, with a split point.
    h.transport->server_send(
        0, transition_page({0, TS0}, {1, TS1}, 0,
                           page_value({"a", "b", "c", "d"}, false, "c1", "SplitRequired", "s1"),
                           "j0"));
    // Two halves are subscribed; the split runs in the pumped page callback.
    ASSERT_TRUE(pump_until(h.c, [&] { return h.transport->sent(0).size() >= 4; }));

    const json first_opts = h.opts_for(1);
    const json second_opts = h.opts_for(2);
    ASSERT_FALSE(first_opts.empty());
    ASSERT_FALSE(second_opts.empty());
    // (beginning, s1] and (s1, c1]: together exactly the original range.
    EXPECT_TRUE(first_opts["cursor"].is_null());
    EXPECT_EQ(first_opts["endCursor"], "s1");
    EXPECT_EQ(second_opts["cursor"], "s1");
    EXPECT_EQ(second_opts["endCursor"], "c1");
    EXPECT_EQ(first_opts["numItems"].get<double>(), 2.0);
    EXPECT_EQ(first_opts["id"].get<double>(), h.pagination_id(0))
        << "a split stays inside the same pagination session";

    // Until both halves load, the original page stays live and visible.
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b", "c", "d"}));
    EXPECT_EQ(h.pq.snapshot().status, pagination_status::can_load_more);
    EXPECT_FALSE(h.sent_remove(0));

    h.transport->server_send(
        0, transition_page({1, TS1}, {2, TS2}, 1, page_value({"a", "b"}, false, "s1"), "j1"));
    h.settle();
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b", "c", "d"}))
        << "half loaded: no partial swap, no duplicated items";
    EXPECT_FALSE(h.sent_remove(0));

    h.transport->server_send(
        0, transition_page({2, TS2}, {3, TS3}, 2, page_value({"c", "d"}, false, "c1"), "j2"));
    ASSERT_TRUE(pump_until(h.c, [&] { return h.sent_remove(0); }));
    const auto snap = h.pq.snapshot();
    EXPECT_EQ(snap.results, items({"a", "b", "c", "d"})) << "same items, now over two pages";
    EXPECT_EQ(snap.status, pagination_status::can_load_more);

    // The list keeps growing from the second half's cursor.
    ASSERT_TRUE(h.pq.load_more(2));
    EXPECT_EQ(h.opts_for(3)["cursor"], "c1");
}

TEST(Paginated, SplitFirstHalfStartsAtTheOriginalPagesCursor) {
    // Regression guard for get-convex/convex-js#54981: convex-js sent a null
    // cursor for the first half, so splitting any page but the first re-read
    // the list from the beginning and duplicated everything before it.
    harness h;
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                page_value({"a", "b"}, false, "c1"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    ASSERT_TRUE(h.pq.load_more(2));
    ASSERT_TRUE(h.transport->wait_for_sent(0, 3));

    // The second page (which starts at c1) grew and needs splitting.
    h.transport->server_send(
        0, transition_page({1, TS1}, {2, TS2}, 1,
                           page_value({"c", "d", "e"}, false, "c2", "SplitRecommended", "s2"),
                           "j1"));
    ASSERT_TRUE(pump_until(h.c, [&] { return h.transport->sent(0).size() >= 5; }));

    const json first_opts = h.opts_for(2);
    EXPECT_FALSE(first_opts["cursor"].is_null()) << "must not restart from the beginning";
    EXPECT_EQ(first_opts["cursor"], "c1");
    EXPECT_EQ(first_opts["endCursor"], "s2");
    EXPECT_EQ(h.opts_for(3)["cursor"], "s2");

    h.transport->server_send(
        0, transition_page({2, TS2}, {3, TS3}, 2, page_value({"c", "d"}, false, "s2"), "j2"));
    h.transport->server_send(
        0, transition_page({3, TS3}, {4, TS4}, 3, page_value({"e"}, true, "c2"), "j3"));
    ASSERT_TRUE(h.wait_status(pagination_status::exhausted));
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b", "c", "d", "e"}));
}

TEST(Paginated, OversizedPageSplitsWithoutServerAsking) {
    // convex-js splits any page that grew past twice the requested size,
    // whatever pageStatus says.
    harness h;  // initial_num_items = 2
    h.transport->server_send(
        0, transition_page({0, TS0}, {1, TS1}, 0,
                           page_value({"a", "b", "c", "d", "e"}, false, "c1", nullptr, "s1"),
                           "j0"));
    ASSERT_TRUE(pump_until(h.c, [&] { return h.transport->sent(0).size() >= 4; }));
    EXPECT_EQ(h.opts_for(1)["endCursor"], "s1");
    EXPECT_EQ(h.opts_for(2)["cursor"], "s1");
}

TEST(Paginated, PageWithinSizeAndNoSplitCursorIsLeftAlone) {
    harness h;
    h.transport->server_send(
        0, transition_page({0, TS0}, {1, TS1}, 0,
                           page_value({"a", "b", "c"}, false, "c1", "SplitRecommended"), "j0"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b", "c"}));
    EXPECT_EQ(h.transport->sent(0).size(), 2u) << "nothing to split on: no extra traffic";
}

TEST(Paginated, SplitRequiredWithoutSplitCursorResetsPagination) {
    harness h;
    // Incomplete, but too few items to have a split point: the only fix is
    // to re-fetch right-sized pages.
    h.transport->server_send(
        0, transition_page({0, TS0}, {1, TS1}, 0,
                           page_value({"a"}, false, "c1", "SplitRequired"), "j0"));
    ASSERT_TRUE(pump_until(h.c, [&] { return h.transport->sent(0).size() >= 4; }));
    EXPECT_EQ(h.pq.snapshot().status, pagination_status::loading_first_page);
    EXPECT_TRUE(h.sent_remove(0));
    const json opts = h.opts_for(1);
    EXPECT_TRUE(opts["cursor"].is_null());
    EXPECT_FALSE(opts.contains("endCursor"));
    EXPECT_NE(opts["id"].get<double>(), h.pagination_id(0)) << "a reset is a new session";

    h.transport->server_send(
        0, transition_page({1, TS1}, {3, TS2}, 1, page_value({"a", "b"}, false, "e1"), "j1"));
    ASSERT_TRUE(h.wait_status(pagination_status::can_load_more));
    EXPECT_EQ(h.pq.snapshot().results, items({"a", "b"}));
}

TEST(Paginated, RepeatedUnsplittablePageStopsResettingAndErrors) {
    harness h;
    // Every re-fetch produces the same unsplittable incomplete page. Reset
    // is capped so this cannot spin on the server forever.
    const json bad = page_value({"a"}, false, "c1", "SplitRequired");
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0, bad, "j0"));
    ASSERT_TRUE(pump_until(h.c, [&] { return !h.add_for(1).empty(); }));
    h.transport->server_send(0, transition_page({1, TS1}, {2, TS2}, 1, bad, "j1"));
    ASSERT_TRUE(pump_until(h.c, [&] { return !h.add_for(2).empty(); }));
    h.transport->server_send(0, transition_page({2, TS2}, {3, TS3}, 2, bad, "j2"));

    ASSERT_TRUE(h.wait_status(pagination_status::error));
    const auto snap = h.pq.snapshot();
    ASSERT_TRUE(snap.error.has_value());
    EXPECT_NE(snap.error->error_message().find("SplitRequired"), std::string::npos);
    EXPECT_NE(snap.error->error_message().find("initial_num_items"), std::string::npos);
    const std::size_t sent = h.transport->sent(0).size();
    h.settle();
    EXPECT_EQ(h.transport->sent(0).size(), sent) << "no further reset traffic";

    // reset() clears the give-up state and tries again.
    h.pq.reset();
    EXPECT_EQ(h.pq.snapshot().status, pagination_status::loading_first_page);
    ASSERT_TRUE(pump_until(h.c, [&] { return !h.add_for(3).empty(); }));
    h.transport->server_send(
        0, transition_page({3, TS3}, {4, TS4}, 3, page_value({"x"}, true, "d1"), "k0"));
    ASSERT_TRUE(h.wait_status(pagination_status::exhausted));
    EXPECT_EQ(h.pq.snapshot().results, items({"x"}));
}

TEST(Paginated, FailedSplitHalfKeepsTheOriginalPage) {
    harness h;
    h.transport->server_send(
        0, transition_page({0, TS0}, {1, TS1}, 0,
                           page_value({"a", "b", "c", "d"}, false, "c1", "SplitRequired", "s1"),
                           "j0"));
    ASSERT_TRUE(pump_until(h.c, [&] { return !h.add_for(2).empty(); }));

    h.transport->server_send(
        0, transition_page({1, TS1}, {2, TS2}, 1, page_value({"a", "b"}, false, "s1"), "j1"));
    h.transport->server_send(
        0, transition_failed({2, TS2}, {3, TS3}, 2, "Server Error: Uncaught Error: boom"));
    // Both half subscriptions are dropped; the original page keeps serving.
    ASSERT_TRUE(pump_until(h.c, [&] { return h.sent_remove(1) && h.sent_remove(2); }));
    const auto snap = h.pq.snapshot();
    EXPECT_EQ(snap.status, pagination_status::can_load_more) << "a failed split is not an error";
    EXPECT_EQ(snap.results, items({"a", "b", "c", "d"}));
    EXPECT_FALSE(h.sent_remove(0));

    // The same page updating again must not retry the failing split.
    const std::size_t sent = h.transport->sent(0).size();
    h.transport->server_send(
        0, transition_page({3, TS3}, {4, TS4}, 0,
                           page_value({"a", "b", "c", "d", "e"}, false, "c1", "SplitRequired",
                                      "s1"),
                           "j0b"));
    ASSERT_TRUE(pump_until(h.c, [&] { return h.pq.snapshot().results.size() == 5; }));
    EXPECT_EQ(h.transport->sent(0).size(), sent) << "no retry of the failed split";
}

TEST(Paginated, InvalidCursorInASplitHalfResetsPagination) {
    harness h;
    h.transport->server_send(
        0, transition_page({0, TS0}, {1, TS1}, 0,
                           page_value({"a", "b", "c", "d"}, false, "c1", "SplitRequired", "s1"),
                           "j0"));
    ASSERT_TRUE(pump_until(h.c, [&] { return !h.add_for(2).empty(); }));

    h.transport->server_send(
        0, transition_failed({1, TS1}, {2, TS2}, 1,
                             "InvalidCursor: Tried to run a query starting from a cursor "
                             "created by a different query"));
    ASSERT_TRUE(h.wait_status(pagination_status::loading_first_page));
    EXPECT_TRUE(h.pq.snapshot().results.empty());
    ASSERT_TRUE(pump_until(h.c, [&] { return !h.add_for(3).empty(); }));
    const json opts = h.opts_for(3);
    EXPECT_TRUE(opts["cursor"].is_null());
    EXPECT_FALSE(opts.contains("endCursor"));
    EXPECT_NE(opts["id"].get<double>(), h.pagination_id(0));
}

TEST(Paginated, SplitPagesResubscribeWithTheirEndCursorsAfterReconnect) {
    harness h;
    h.transport->server_send(
        0, transition_page({0, TS0}, {1, TS1}, 0,
                           page_value({"a", "b", "c", "d"}, false, "c1", "SplitRequired", "s1"),
                           "j0"));
    ASSERT_TRUE(pump_until(h.c, [&] { return !h.add_for(2).empty(); }));
    h.transport->server_send(
        0, transition_page({1, TS1}, {2, TS2}, 1, page_value({"a", "b"}, false, "s1"), "j1"));
    h.transport->server_send(
        0, transition_page({2, TS2}, {3, TS3}, 2, page_value({"c", "d"}, false, "c1"), "j2"));
    ASSERT_TRUE(pump_until(h.c, [&] { return h.sent_remove(0); }));

    h.transport->server_close(0, "SimulatedNetworkBlip");
    ASSERT_TRUE(h.transport->wait_for_attempts(2));
    h.transport->open(1);
    ASSERT_TRUE(h.transport->wait_for_sent(1, 2));

    const json mqs = h.sent_json(1, 1);
    ASSERT_EQ(mqs["modifications"].size(), 2u) << "only the two halves are active";
    for (const json& mod : mqs["modifications"]) {
        const json& opts = mod["args"][0]["paginationOpts"];
        ASSERT_TRUE(opts.contains("endCursor")) << "split bounds survive a reconnect";
        if (mod["queryId"] == 1) {
            EXPECT_TRUE(opts["cursor"].is_null());
            EXPECT_EQ(opts["endCursor"], "s1");
            EXPECT_EQ(mod["journal"], "j1");
        } else {
            EXPECT_EQ(mod["queryId"], 2);
            EXPECT_EQ(opts["cursor"], "s1");
            EXPECT_EQ(opts["endCursor"], "c1");
            EXPECT_EQ(mod["journal"], "j2");
        }
    }
}

TEST(Paginated, MalformedPaginationResultIsAnError) {
    harness h;
    h.transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0, json(42.0)));
    ASSERT_TRUE(h.wait_status(pagination_status::error));
    ASSERT_TRUE(h.pq.snapshot().error.has_value());
    EXPECT_NE(h.pq.snapshot().error->error_message().find("PaginationResult"),
              std::string::npos);
}

TEST(Paginated, DestructionUnsubscribesAllPages) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));
    {
        paginated_query pq(
            c, paginated_query::options{"messages:listPaginated", {}, 2}, nullptr);
        ASSERT_TRUE(transport->wait_for_attempts(1));
        transport->open(0);
        ASSERT_TRUE(transport->wait_for_sent(0, 2));
        transport->server_send(0, transition_page({0, TS0}, {1, TS1}, 0,
                                                  page_value({"a"}, false, "c1"), "j0"));
        ASSERT_TRUE(pump_until(c, [&] {
            return pq.snapshot().status == pagination_status::can_load_more;
        }));
        ASSERT_TRUE(pq.load_more(2));
        ASSERT_TRUE(transport->wait_for_sent(0, 3));
    }
    ASSERT_TRUE(transport->wait_for_sent(0, 5));
    EXPECT_EQ(json::parse(transport->sent(0)[3])["modifications"][0]["type"], "Remove");
    EXPECT_EQ(json::parse(transport->sent(0)[4])["modifications"][0]["type"], "Remove");
    c.process_events();  // queued page callbacks after destruction must no-op
}

TEST(Paginated, InvalidOptionsThrow) {
    auto transport = std::make_shared<mock_transport>();
    client c(make_options(transport));
    EXPECT_THROW(paginated_query(c, {"messages:listPaginated", {}, 0}, nullptr),
                 std::invalid_argument);
    EXPECT_THROW(paginated_query(c, {"", {}, 5}, nullptr), std::invalid_argument);
}
