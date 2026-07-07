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
#include <convex/json_codec.h>
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
