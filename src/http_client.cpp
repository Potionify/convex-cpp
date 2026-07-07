#include <convex/http_client.h>

#include "detail/wire_json.h"

namespace convex {

namespace {

using nlohmann::json;

function_result parse_function_response(const http_response& response) {
    if (!response.error.empty()) {
        return function_result::error("convex: transport error: " + response.error);
    }
    json body;
    try {
        body = json::parse(response.body);
    } catch (const json::exception&) {
        return function_result::error("convex: HTTP " + std::to_string(response.status) +
                                      " with unparsable body");
    }
    const auto status_it = body.find("status");
    if (status_it == body.end() || !status_it->is_string()) {
        return function_result::error("convex: HTTP " + std::to_string(response.status) +
                                      ": response has no status field");
    }
    try {
        if (*status_it == "success") {
            return function_result::success(detail::json_to_value(body.at("value")));
        }
        // Application/developer error. HTTP status 560 marks a UDF error; the
        // body shape is the same either way.
        std::string message = body.value("errorMessage", "unknown Convex error");
        const auto data_it = body.find("errorData");
        if (data_it != body.end() && !data_it->is_null()) {
            return function_result::error(
                convex_error{std::move(message), detail::json_to_value(*data_it)});
        }
        return function_result::error(std::move(message));
    } catch (const std::exception& e) {
        return function_result::error(std::string("convex: malformed response: ") + e.what());
    }
}

std::future<function_result> as_future(
    http_client& c, void (http_client::*method)(std::string_view, value_object,
                                                http_client::result_callback),
    std::string_view path, value_object args) {
    auto promise = std::make_shared<std::promise<function_result>>();
    auto future = promise->get_future();
    (c.*method)(path, std::move(args),
                [promise](function_result r) { promise->set_value(std::move(r)); });
    return future;
}

}  // namespace

http_client::http_client(std::string deployment_url, std::shared_ptr<http_transport> transport)
    : url_(std::move(deployment_url)), transport_(std::move(transport)) {
    if (!transport_) throw std::invalid_argument("convex: http transport required");
    while (!url_.empty() && url_.back() == '/') url_.pop_back();
    if (url_.find("://") == std::string::npos) {
        throw std::invalid_argument("convex: deployment URL has no scheme: " + url_);
    }
}

void http_client::set_auth(auth_token token) {
    std::shared_ptr<const std::string> header;
    switch (token.type) {
        case auth_token::kind::none:
            break;
        case auth_token::kind::user:
            header = std::make_shared<const std::string>("Bearer " + token.value);
            break;
        case auth_token::kind::admin:
            header = std::make_shared<const std::string>("Convex " + token.value);
            break;
    }
    std::lock_guard lk(mu_);
    auth_header_ = std::move(header);
}

void http_client::call(const char* endpoint, std::string_view udf_path, value_object args,
                       result_callback on_done) {
    json body{{"path", canonicalize_udf_path(udf_path)},
              {"format", "convex_encoded_json"},
              {"args", json::parse(serialize_args(args))}};

    http_request request;
    request.method = "POST";
    request.url = url_ + endpoint;
    request.headers["Content-Type"] = "application/json";
    {
        std::lock_guard lk(mu_);
        if (auth_header_) request.headers["Authorization"] = *auth_header_;
    }
    request.body = body.dump();

    transport_->send(std::move(request), [on_done = std::move(on_done)](http_response response) {
        on_done(parse_function_response(response));
    });
}

void http_client::query(std::string_view udf_path, value_object args, result_callback on_done) {
    call("/api/query", udf_path, std::move(args), std::move(on_done));
}
void http_client::mutation(std::string_view udf_path, value_object args, result_callback on_done) {
    call("/api/mutation", udf_path, std::move(args), std::move(on_done));
}
void http_client::action(std::string_view udf_path, value_object args, result_callback on_done) {
    call("/api/action", udf_path, std::move(args), std::move(on_done));
}

std::future<function_result> http_client::query(std::string_view udf_path, value_object args) {
    return as_future(*this,
                     static_cast<void (http_client::*)(std::string_view, value_object,
                                                       result_callback)>(&http_client::query),
                     udf_path, std::move(args));
}
std::future<function_result> http_client::mutation(std::string_view udf_path, value_object args) {
    return as_future(*this,
                     static_cast<void (http_client::*)(std::string_view, value_object,
                                                       result_callback)>(&http_client::mutation),
                     udf_path, std::move(args));
}
std::future<function_result> http_client::action(std::string_view udf_path, value_object args) {
    return as_future(*this,
                     static_cast<void (http_client::*)(std::string_view, value_object,
                                                       result_callback)>(&http_client::action),
                     udf_path, std::move(args));
}

}  // namespace convex
