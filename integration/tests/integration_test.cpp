// Live integration tests against a real Convex backend running the schema in
// integration/convex-test-project. Start it first:
//   cd integration/backend && docker compose up -d
//
// Environment:
//   CONVEX_LOCAL_URL       backend URL   (default http://127.0.0.1:3210)
//   CONVEX_LOCAL_ADMIN_KEY enables the admin-auth test when set
//   CONVEX_CLOUD_URL       enables the TLS smoke test against a cloud dev
//                          deployment when set (wss://)

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <convex/client.h>
#include <convex/file_storage.h>
#include <convex/http_client.h>
#include <convex/json_codec.h>
#include <convex/paginated.h>
#include <gtest/gtest.h>

#include "ixwebsocket/ixwebsocket_transport.h"

using namespace convex;
using namespace std::chrono_literals;

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name); v != nullptr && *v != '\0') return v;
    return fallback;
}

std::string local_url() { return env_or("CONVEX_LOCAL_URL", "http://127.0.0.1:3210"); }

client make_client(const std::string& url) {
    client_options o;
    o.deployment_url = url;
    o.websocket = transports::make_ixwebsocket_transport();
    o.delivery_mode = client_options::delivery::immediate;
    return client(std::move(o));
}

/// Collects subscription updates and lets tests wait for the n-th one.
class update_collector {
public:
    client::update_callback callback() {
        return [this](const function_result& r) {
            std::lock_guard lk(mu_);
            updates_.push_back(r);
            cv_.notify_all();
        };
    }

    bool wait_for(std::size_t count, std::chrono::milliseconds timeout = 15s) {
        std::unique_lock lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return updates_.size() >= count; });
    }

    std::vector<function_result> snapshot() {
        std::lock_guard lk(mu_);
        return updates_;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<function_result> updates_;
};

function_result await(std::future<function_result>& f,
                      std::chrono::milliseconds timeout = 15s) {
    if (f.wait_for(timeout) != std::future_status::ready) {
        throw std::runtime_error("timed out waiting for function result");
    }
    return f.get();
}

std::string unique_name(const char* prefix) {
    return std::string(prefix) + "-" +
           std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
}

}  // namespace

TEST(Live, KitchenSinkCoversWholeValueSpace) {
    auto c = make_client(local_url());
    auto future = c.query("values:kitchenSink", {});
    const auto result = await(future);
    ASSERT_TRUE(result.ok()) << result.error_message();
    const value_object& sink = result.get_value().as_object();

    EXPECT_TRUE(sink.at("nullValue").is_null());
    EXPECT_EQ(sink.at("boolTrue"), value(true));
    EXPECT_EQ(sink.at("boolFalse"), value(false));

    // Int64 fidelity: 2^53+1 is not representable as a double; INT64_MIN
    // would have been destroyed by the old plugin's number handling.
    EXPECT_EQ(sink.at("int64Big"), value(std::int64_t{9007199254740993}));
    EXPECT_EQ(sink.at("int64Min"), value(std::numeric_limits<std::int64_t>::min()));

    EXPECT_EQ(sink.at("float"), value(1.5));
    EXPECT_EQ(sink.at("floatZero"), value(0.0));

    const value_array& specials = sink.at("specialFloats").as_array();
    ASSERT_EQ(specials.size(), 4u);
    EXPECT_TRUE(std::isnan(specials[0].as_float64()));
    EXPECT_EQ(specials[1].as_float64(), std::numeric_limits<double>::infinity());
    EXPECT_EQ(specials[2].as_float64(), -std::numeric_limits<double>::infinity());
    EXPECT_TRUE(specials[3].as_float64() == 0.0 && std::signbit(specials[3].as_float64()));

    // The source string ends with a decomposed n + combining tilde (NFD);
    // the codec must preserve it byte-exactly, not normalize.
    EXPECT_EQ(sink.at("unicode").as_string(),
              "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\xA7\xAA\xE2\x9C\xA8 "
              "caf\xC3\xA9 n\xCC\x83");
    EXPECT_EQ(sink.at("bytes"), value(bytes{0, 1, 2, 3, 4, 5, 6, 7}));

    const value_array& nested = sink.at("nestedArray").as_array();
    EXPECT_EQ(nested[0], value(1.0));
    EXPECT_EQ(nested[2], value("six"));
    EXPECT_EQ(sink.at("nestedObject")
                  .as_object()
                  .at("b")
                  .as_object()
                  .at("d")
                  .as_object()
                  .at("f")
                  .as_array()[2],
              value(9.0));
}

TEST(Live, EchoMutationRoundTripsEveryType) {
    auto c = make_client(local_url());
    const value payload(value_object{
        {"int64Max", value(std::numeric_limits<std::int64_t>::max())},
        {"int64Min", value(std::numeric_limits<std::int64_t>::min())},
        {"bytes", value(bytes{0xff, 0x00, 0x7f, 0x80})},
        {"specials", value(value_array{value(std::numeric_limits<double>::quiet_NaN()),
                                       value(std::numeric_limits<double>::infinity()), value(-0.0)})},
        {"unicode", value("\xF0\x9F\x8E\xAE \xE6\xB8\xB8\xE6\x88\x8F")},
        {"nested", value(value_object{{"list", value(value_array{value(1), value("two"),
                                                                 value(true), value(nullptr)})}})},
    });

    auto future = c.mutation("values:echoMutation", {{"x", payload}});
    const auto result = await(future);
    ASSERT_TRUE(result.ok()) << result.error_message();
    // Wire-encoding comparison sidesteps NaN != NaN while remaining bit-exact.
    EXPECT_EQ(to_wire_json(result.get_value()), to_wire_json(payload));
}

TEST(Live, SubscriptionSeesMutationsWithReadYourWrites) {
    auto c = make_client(local_url());
    const std::string name = unique_name("counter");

    update_collector updates;
    auto sub = c.subscribe("counters:get", {{"name", value(name)}}, updates.callback());

    // Initial value: the counter does not exist yet.
    ASSERT_TRUE(updates.wait_for(1));
    EXPECT_TRUE(updates.snapshot()[0].get_value().is_null());

    // Increment. When the mutation future resolves, the subscription MUST
    // already have observed the new value (read-your-writes ordering).
    auto inc = c.mutation("counters:increment", {{"name", value(name)}});
    const auto inc_result = await(inc);
    ASSERT_TRUE(inc_result.ok()) << inc_result.error_message();
    EXPECT_EQ(inc_result.get_value(), value(1.0));
    const auto seen = updates.snapshot();
    ASSERT_GE(seen.size(), 2u) << "mutation resolved before its effects were visible";
    EXPECT_EQ(seen.back().get_value(), value(1.0));

    auto inc2 = c.mutation("counters:increment", {{"name", value(name)}, {"by", value(5.0)}});
    EXPECT_EQ(await(inc2).get_value(), value(6.0));
    ASSERT_TRUE(updates.wait_for(3));
    EXPECT_EQ(updates.snapshot().back().get_value(), value(6.0));
}

TEST(Live, MessagesListOrdered) {
    auto c = make_client(local_url());
    const std::string channel = unique_name("channel");

    auto m1 = c.mutation("messages:send", {{"channel", value(channel)},
                                           {"author", value("cpp")},
                                           {"body", value("first")}});
    (void)await(m1);
    auto m2 = c.mutation("messages:send", {{"channel", value(channel)},
                                           {"author", value("cpp")},
                                           {"body", value("second")}});
    (void)await(m2);

    auto future = c.query("messages:list", {{"channel", value(channel)}});
    const auto result = await(future);
    ASSERT_TRUE(result.ok()) << result.error_message();
    const value_array& messages = result.get_value().as_array();
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].as_object().at("body"), value("first"));
    EXPECT_EQ(messages[1].as_object().at("body"), value("second"));
    // System fields come through as real values.
    EXPECT_TRUE(messages[0].as_object().at("_creationTime").is_float64());
    EXPECT_TRUE(messages[0].as_object().at("_id").is_string());
}

TEST(Live, PaginatedQueryChainsPagesAndSeesLiveInserts) {
    auto c = make_client(local_url());
    const std::string channel = unique_name("paged");

    auto send = [&](const std::string& body) {
        auto m = c.mutation("messages:send", {{"channel", value(channel)},
                                              {"author", value("cpp")},
                                              {"body", value(body)}});
        ASSERT_TRUE(await(m).ok());
    };
    for (int i = 1; i <= 5; ++i) send("m" + std::to_string(i));

    std::mutex mu;
    std::condition_variable cv;
    std::vector<paginated_snapshot> snaps;
    paginated_query pq(
        c, {"messages:listPaginated", {{"channel", value(channel)}}, /*initial_num_items=*/2},
        [&](const paginated_snapshot& s) {
            std::lock_guard lk(mu);
            snaps.push_back(s);
            cv.notify_all();
        });
    auto wait_snap = [&](auto pred) {
        std::unique_lock lk(mu);
        return cv.wait_for(lk, 15s, [&] { return !snaps.empty() && pred(snaps.back()); });
    };
    auto bodies = [](const paginated_snapshot& s) {
        std::vector<std::string> out;
        for (const value& doc : s.results) out.push_back(doc.as_object().at("body").as_string());
        return out;
    };

    // First page: 2 of 5, more available.
    ASSERT_TRUE(wait_snap([](const paginated_snapshot& s) {
        return s.status == pagination_status::can_load_more && s.results.size() == 2;
    }));
    EXPECT_EQ(bodies(pq.snapshot()), (std::vector<std::string>{"m1", "m2"}));

    // Second page chains at the real server cursor.
    ASSERT_TRUE(pq.load_more(2));
    ASSERT_TRUE(wait_snap([](const paginated_snapshot& s) {
        return s.status == pagination_status::can_load_more && s.results.size() == 4;
    }));

    // Third page drains the list.
    ASSERT_TRUE(pq.load_more(10));
    ASSERT_TRUE(wait_snap(
        [](const paginated_snapshot& s) { return s.status == pagination_status::exhausted; }));
    EXPECT_EQ(bodies(pq.snapshot()),
              (std::vector<std::string>{"m1", "m2", "m3", "m4", "m5"}));

    // Live update: a new message lands in the (unbounded) last page without
    // disturbing earlier page boundaries.
    send("m6");
    ASSERT_TRUE(wait_snap([](const paginated_snapshot& s) { return s.results.size() == 6; }));
    const auto final_snap = pq.snapshot();
    EXPECT_EQ(final_snap.status, pagination_status::exhausted);
    EXPECT_EQ(bodies(final_snap),
              (std::vector<std::string>{"m1", "m2", "m3", "m4", "m5", "m6"}));
}

TEST(Live, ConvexErrorCarriesData) {
    auto c = make_client(local_url());
    auto future = c.query("errors:throwConvexError", {});
    const auto result = await(future);
    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.is_app_error()) << result.error_message();
    const value_object& data = result.app_error()->data.as_object();
    EXPECT_EQ(data.at("code"), value("TEST"));
    const value_array& details = data.at("details").as_array();
    ASSERT_EQ(details.size(), 2u);
    EXPECT_EQ(details[0], value(1.0));
    EXPECT_EQ(details[1], value("two"));

    auto plain = c.query("errors:throwPlainError", {});
    const auto plain_result = await(plain);
    ASSERT_FALSE(plain_result.ok());
    EXPECT_FALSE(plain_result.is_app_error());
}

TEST(Live, ActionsRunAndEcho) {
    auto c = make_client(local_url());
    const value payload(value_object{{"n", value(std::int64_t{42})}, {"s", value("echo")}});
    auto future = c.action("actions:echoAction", {{"x", payload}});
    const auto result = await(future);
    ASSERT_TRUE(result.ok()) << result.error_message();
    EXPECT_EQ(to_wire_json(result.get_value()), to_wire_json(payload));

    auto now_future = c.action("actions:now", {});
    const auto now_result = await(now_future);
    ASSERT_TRUE(now_result.ok());
    EXPECT_GT(now_result.get_value().as_float64(), 1.7e12);  // sane wall clock (ms)
}

TEST(Live, AdminAuthenticate) {
    const std::string key = env_or("CONVEX_LOCAL_ADMIN_KEY", "");
    if (key.empty()) GTEST_SKIP() << "CONVEX_LOCAL_ADMIN_KEY not set";

    auto c = make_client(local_url());
    c.set_auth(auth_token::admin(key));
    // A bad key would produce AuthError -> reconnect loop and this query
    // would never resolve; success proves Authenticate was accepted.
    auto future = c.query("counters:get", {{"name", value("auth-smoke")}});
    const auto result = await(future);
    EXPECT_TRUE(result.ok()) << result.error_message();
}

TEST(LiveHttp, QueryMutationActionAndErrors) {
    http_client c(local_url(), transports::make_ixwebsocket_http_transport());

    // Query with full value fidelity.
    auto sink_future = c.query("values:kitchenSink", {});
    const auto sink = await(sink_future);
    ASSERT_TRUE(sink.ok()) << sink.error_message();
    EXPECT_EQ(sink.get_value().as_object().at("int64Big"),
              value(std::int64_t{9007199254740993}));

    // Mutation round trip (echo).
    const value payload(value_object{{"n", value(std::numeric_limits<std::int64_t>::min())},
                                     {"b", value(bytes{1, 2, 3})}});
    auto echo_future = c.mutation("values:echoMutation", {{"x", payload}});
    const auto echo = await(echo_future);
    ASSERT_TRUE(echo.ok()) << echo.error_message();
    EXPECT_EQ(to_wire_json(echo.get_value()), to_wire_json(payload));

    // Action.
    auto now_future = c.action("actions:now", {});
    EXPECT_GT(await(now_future).get_value().as_float64(), 1.7e12);

    // ConvexError data survives the HTTP path (status 560).
    auto err_future = c.query("errors:throwConvexError", {});
    const auto err = await(err_future);
    ASSERT_FALSE(err.ok());
    ASSERT_TRUE(err.is_app_error()) << err.error_message();
    EXPECT_EQ(err.app_error()->data.as_object().at("code"), value("TEST"));

    // Unknown function is a plain error, not a transport failure. (The
    // local backend redacts the detail to "Server Error"; cloud spells out
    // "Could not find public function".)
    auto missing_future = c.query("nope:missing", {});
    const auto missing = await(missing_future);
    ASSERT_FALSE(missing.ok());
    EXPECT_FALSE(missing.is_app_error());
    EXPECT_FALSE(missing.error_message().empty());
}

TEST(LiveHttp, FileStorageRoundTrip) {
    auto transport = transports::make_ixwebsocket_http_transport();
    http_client c(local_url(), transport);

    // 1. Generate an upload URL.
    auto url_future = c.mutation("files:generateUploadUrl", {});
    const auto url_result = await(url_future);
    ASSERT_TRUE(url_result.ok()) << url_result.error_message();
    const std::string upload_url = url_result.get_value().as_string();

    // 2. Upload bytes (with a few non-UTF8 values to prove binary safety).
    bytes payload{0x00, 0x01, 0xff, 0xfe, 'c', 'o', 'n', 'v', 'e', 'x', 0x80};
    auto store_future = store_file(*transport, upload_url, "application/octet-stream", payload);
    const auto stored = await(store_future);
    ASSERT_TRUE(stored.ok()) << stored.error_message();
    const std::string storage_id = stored.get_value().as_object().at("storageId").as_string();
    EXPECT_FALSE(storage_id.empty());

    // 3. Metadata reflects the upload.
    auto meta_future = c.query("files:getMetadata", {{"storageId", value(storage_id)}});
    const auto meta = await(meta_future);
    ASSERT_TRUE(meta.ok()) << meta.error_message();
    EXPECT_EQ(meta.get_value().as_object().at("size"),
              value(static_cast<double>(payload.size())));

    // 4. Resolve a download URL and fetch the bytes back.
    auto get_url_future = c.query("files:getUrl", {{"storageId", value(storage_id)}});
    const auto get_url = await(get_url_future);
    ASSERT_TRUE(get_url.ok()) << get_url.error_message();
    auto fetch_future = fetch_file(*transport, get_url.get_value().as_string());
    const auto fetched = await(fetch_future);
    ASSERT_TRUE(fetched.ok()) << fetched.error_message();
    EXPECT_EQ(fetched.get_value(), value(payload));
}

TEST(Live, CloudTlsSmoke) {
    const std::string cloud = env_or("CONVEX_CLOUD_URL", "");
    if (cloud.empty()) GTEST_SKIP() << "CONVEX_CLOUD_URL not set";

    auto c = make_client(cloud);
    auto future = c.query("values:kitchenSink", {});
    const auto result = await(future, 30s);
    ASSERT_TRUE(result.ok()) << result.error_message();
    EXPECT_EQ(result.get_value().as_object().at("int64Big"),
              value(std::int64_t{9007199254740993}));
}
