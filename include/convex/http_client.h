#pragma once

// One-shot Convex calls over plain HTTP (POST /api/query|mutation|action).
// No connection state, no subscriptions, no read-your-writes ordering —
// use convex::client for realtime work. Handy for tools, CLIs, and simple
// request/response use.

#include <convex/error.h>
#include <convex/protocol.h>
#include <convex/transport.h>
#include <convex/value.h>

#include <functional>
#include <future>
#include <memory>
#include <string>

namespace convex {

class http_client {
public:
    using result_callback = std::function<void(function_result)>;

    /// `deployment_url` like "https://x.convex.cloud"; `transport` performs
    /// the actual requests (e.g. transports::make_ixwebsocket_http_transport()).
    http_client(std::string deployment_url, std::shared_ptr<http_transport> transport);

    /// Set or clear (auth_token::none()) the token sent as Authorization:
    /// "Bearer <jwt>" for user tokens, "Convex <key>" for admin keys.
    void set_auth(auth_token token);

    void query(std::string_view udf_path, value_object args, result_callback on_done);
    void mutation(std::string_view udf_path, value_object args, result_callback on_done);
    void action(std::string_view udf_path, value_object args, result_callback on_done);

    std::future<function_result> query(std::string_view udf_path, value_object args);
    std::future<function_result> mutation(std::string_view udf_path, value_object args);
    std::future<function_result> action(std::string_view udf_path, value_object args);

    const std::string& deployment_url() const { return url_; }
    http_transport& transport() { return *transport_; }

private:
    void call(const char* endpoint, std::string_view udf_path, value_object args,
              result_callback on_done);

    std::string url_;  // normalized, no trailing slash
    std::shared_ptr<http_transport> transport_;
    std::shared_ptr<const std::string> auth_header_;  // swapped atomically under mu_
    std::mutex mu_;
};

}  // namespace convex
