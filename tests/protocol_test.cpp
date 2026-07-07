// Wire-shape tests for the sync protocol codec. Expected JSON matches
// convex-js src/browser/sync/protocol.ts (fields alphabetized here because
// the encoder emits sorted keys; servers do not care about field order).

#include <convex/protocol.h>
#include <gtest/gtest.h>

using namespace convex;

// ------------------------------------------------------------ udf paths

TEST(UdfPath, Canonicalization) {
    EXPECT_EQ(canonicalize_udf_path("messages:list"), "messages:list");
    EXPECT_EQ(canonicalize_udf_path("messages"), "messages:default");
    EXPECT_EQ(canonicalize_udf_path("foo/bar:baz"), "foo/bar:baz");
    EXPECT_EQ(canonicalize_udf_path("foo/bar.js"), "foo/bar:default");
    EXPECT_EQ(canonicalize_udf_path("foo.js:baz"), "foo:baz");
}

TEST(Args, Serialization) {
    EXPECT_EQ(serialize_args({}), "[{}]");
    EXPECT_EQ(serialize_args({{"channel", value("general")}, {"limit", value(10)}}),
              R"([{"channel":"general","limit":{"$integer":"CgAAAAAAAAA="}}])");
}

// ------------------------------------------------------- client encoding

TEST(ClientEncode, Connect) {
    connect_message m;
    m.session_id = "3d1c8e64-7b1f-4d1e-9e5a-1c2b3d4e5f60";
    m.connection_count = 0;
    m.last_close_reason = "InitialConnect";
    EXPECT_EQ(encode_client_message(m),
              R"({"connectionCount":0,"lastCloseReason":"InitialConnect",)"
              R"("sessionId":"3d1c8e64-7b1f-4d1e-9e5a-1c2b3d4e5f60","type":"Connect"})");

    m.connection_count = 3;
    m.max_observed_timestamp = timestamp{1};
    m.client_ts = 1720000000000ull;
    EXPECT_EQ(encode_client_message(m),
              R"({"clientTs":1720000000000,"connectionCount":3,)"
              R"("lastCloseReason":"InitialConnect","maxObservedTimestamp":"AQAAAAAAAAA=",)"
              R"("sessionId":"3d1c8e64-7b1f-4d1e-9e5a-1c2b3d4e5f60","type":"Connect"})");
}

TEST(ClientEncode, ModifyQuerySet) {
    modify_query_set_message m;
    m.base_version = 0;
    m.new_version = 1;
    query_add add;
    add.id = 7;
    add.udf_path = "messages:list";
    add.args_json = serialize_args({{"channel", value("general")}});
    m.modifications.push_back(add);
    m.modifications.push_back(query_remove{5});
    EXPECT_EQ(encode_client_message(m),
              R"({"baseVersion":0,"modifications":[)"
              R"({"args":[{"channel":"general"}],"queryId":7,"type":"Add",)"
              R"("udfPath":"messages:list"},)"
              R"({"queryId":5,"type":"Remove"}],)"
              R"("newVersion":1,"type":"ModifyQuerySet"})");
}

TEST(ClientEncode, MutationAndAction) {
    mutation_request_message mu;
    mu.id = 12;
    mu.udf_path = "messages:send";
    mu.args_json = serialize_args({{"body", value("hi")}});
    EXPECT_EQ(encode_client_message(mu),
              R"({"args":[{"body":"hi"}],"requestId":12,"type":"Mutation",)"
              R"("udfPath":"messages:send"})");

    action_request_message ac;
    ac.id = 13;
    ac.udf_path = "actions:echoAction";
    ac.args_json = serialize_args({});
    EXPECT_EQ(encode_client_message(ac),
              R"({"args":[{}],"requestId":13,"type":"Action","udfPath":"actions:echoAction"})");
}

TEST(ClientEncode, Authenticate) {
    authenticate_message none;
    none.base_version = 0;
    none.token = auth_token::none();
    EXPECT_EQ(encode_client_message(none),
              R"({"baseVersion":0,"tokenType":"None","type":"Authenticate"})");

    authenticate_message user;
    user.base_version = 2;
    user.token = auth_token::user("jwt-token");
    EXPECT_EQ(encode_client_message(user),
              R"({"baseVersion":2,"tokenType":"User","type":"Authenticate","value":"jwt-token"})");

    authenticate_message admin;
    admin.base_version = 0;
    admin.token = auth_token::admin("deploy-key", value_object{{"subject", value("u1")}});
    EXPECT_EQ(encode_client_message(admin),
              R"({"baseVersion":0,"impersonating":{"subject":"u1"},"tokenType":"Admin",)"
              R"("type":"Authenticate","value":"deploy-key"})");
}

// ------------------------------------------------------- server decoding

TEST(ServerDecode, Transition) {
    const auto msg = decode_server_message(R"({
        "type": "Transition",
        "startVersion": {"querySet": 0, "identity": 0, "ts": "AAAAAAAAAAA="},
        "endVersion": {"querySet": 1, "identity": 0, "ts": "AQAAAAAAAAA="},
        "modifications": [
            {"type": "QueryUpdated", "queryId": 7, "value": {"$integer": "KgAAAAAAAAA="},
             "logLines": ["[LOG] hi"], "journal": null},
            {"type": "QueryFailed", "queryId": 8, "errorMessage": "boom",
             "errorData": {"code": "TEST"}, "logLines": [], "journal": null},
            {"type": "QueryRemoved", "queryId": 5}
        ]
    })");
    const auto& t = std::get<transition_message>(msg);
    EXPECT_EQ(t.start_version, (state_version{0, 0, 0}));
    EXPECT_EQ(t.end_version, (state_version{1, 0, 1}));
    ASSERT_EQ(t.modifications.size(), 3u);

    const auto& updated = std::get<query_updated>(t.modifications[0]);
    EXPECT_EQ(updated.id, 7u);
    EXPECT_EQ(updated.result, value(std::int64_t{42}));
    ASSERT_EQ(updated.log_lines.size(), 1u);
    EXPECT_EQ(updated.log_lines[0], "[LOG] hi");
    EXPECT_FALSE(updated.journal.has_value());

    const auto& failed = std::get<query_failed>(t.modifications[1]);
    EXPECT_EQ(failed.id, 8u);
    EXPECT_EQ(failed.error_message, "boom");
    ASSERT_TRUE(failed.error_data.has_value());
    EXPECT_EQ(failed.error_data->as_object().at("code"), value("TEST"));

    EXPECT_EQ(std::get<query_removed>(t.modifications[2]).id, 5u);
}

TEST(ServerDecode, MutationResponseSuccess) {
    const auto msg = decode_server_message(
        R"({"type":"MutationResponse","requestId":12,"success":true,)"
        R"("result":{"$integer":"AQAAAAAAAAA="},"ts":"KgAAAAAAAAA=","logLines":[]})");
    const auto& m = std::get<mutation_response_message>(msg);
    EXPECT_EQ(m.id, 12u);
    ASSERT_TRUE(m.result.ok());
    EXPECT_EQ(m.result.get_value(), value(std::int64_t{1}));
    ASSERT_TRUE(m.ts.has_value());
    EXPECT_EQ(*m.ts, 42u);
}

TEST(ServerDecode, MutationResponseFailure) {
    // Plain failure: no ts, string result.
    const auto plain = decode_server_message(
        R"({"type":"MutationResponse","requestId":12,"success":false,)"
        R"("result":"Server Error","logLines":[]})");
    const auto& p = std::get<mutation_response_message>(plain);
    EXPECT_FALSE(p.result.ok());
    EXPECT_FALSE(p.result.is_app_error());
    EXPECT_EQ(p.result.error_message(), "Server Error");
    EXPECT_FALSE(p.ts.has_value());

    // Application error: errorData present -> convex_error.
    const auto app = decode_server_message(
        R"({"type":"MutationResponse","requestId":13,"success":false,)"
        R"("result":"Uncaught ConvexError","errorData":{"code":"TEST"},"logLines":[]})");
    const auto& a = std::get<mutation_response_message>(app);
    ASSERT_TRUE(a.result.is_app_error());
    EXPECT_EQ(a.result.app_error()->data.as_object().at("code"), value("TEST"));
}

TEST(ServerDecode, ActionResponse) {
    const auto msg = decode_server_message(
        R"({"type":"ActionResponse","requestId":9,"success":true,"result":1.5,"logLines":[]})");
    const auto& a = std::get<action_response_message>(msg);
    EXPECT_EQ(a.id, 9u);
    EXPECT_EQ(a.result.get_value(), value(1.5));
}

TEST(ServerDecode, Errors) {
    const auto auth = decode_server_message(
        R"({"type":"AuthError","error":"bad token","baseVersion":1,"authUpdateAttempted":true})");
    const auto& e = std::get<auth_error_message>(auth);
    EXPECT_EQ(e.error, "bad token");
    EXPECT_EQ(e.base_version, identity_version{1});
    EXPECT_TRUE(e.auth_update_attempted);

    const auto fatal = decode_server_message(R"({"type":"FatalError","error":"nope"})");
    EXPECT_EQ(std::get<fatal_error_message>(fatal).error, "nope");

    const auto ping = decode_server_message(R"({"type":"Ping"})");
    EXPECT_TRUE(std::holds_alternative<ping_message>(ping));
}

TEST(ServerDecode, Malformed) {
    EXPECT_THROW(decode_server_message("not json"), protocol_error);
    EXPECT_THROW(decode_server_message(R"({"noType":1})"), protocol_error);
    EXPECT_THROW(decode_server_message(R"({"type":"TransitionChunk","chunk":"x"})"),
                 protocol_error);
    EXPECT_THROW(decode_server_message(R"({"type":"MutationResponse","requestId":1})"),
                 protocol_error);
    EXPECT_THROW(decode_server_message(
                     R"({"type":"Transition","startVersion":{"querySet":0,"identity":0,)"
                     R"("ts":12345},"endVersion":{"querySet":0,"identity":0,)"
                     R"("ts":"AAAAAAAAAAA="},"modifications":[]})"),
                 protocol_error)
        << "numeric timestamps must be rejected (wire uses base64 strings)";
}

// ---------------------------------------------------------- session ids

TEST(SessionId, UuidV4Shape) {
    const std::string a = generate_session_id();
    const std::string b = generate_session_id();
    EXPECT_NE(a, b);
    ASSERT_EQ(a.size(), 36u);
    EXPECT_EQ(a[8], '-');
    EXPECT_EQ(a[13], '-');
    EXPECT_EQ(a[18], '-');
    EXPECT_EQ(a[23], '-');
    EXPECT_EQ(a[14], '4');                          // version
    EXPECT_TRUE(std::string("89ab").find(a[19]) != std::string::npos);  // variant
}
