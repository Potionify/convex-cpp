#include <convex/file_storage.h>

#include "detail/wire_json.h"

namespace convex {

void store_file(http_transport& transport, const std::string& upload_url,
                const std::string& content_type, bytes data,
                std::function<void(function_result)> on_done) {
    http_request request;
    request.method = "POST";
    request.url = upload_url;
    request.headers["Content-Type"] = content_type;
    request.body.assign(data.begin(), data.end());

    transport.send(std::move(request), [on_done = std::move(on_done)](http_response response) {
        if (!response.error.empty()) {
            on_done(function_result::error("convex: upload transport error: " + response.error));
            return;
        }
        if (response.status < 200 || response.status >= 300) {
            on_done(function_result::error("convex: upload failed with HTTP " +
                                           std::to_string(response.status) + ": " +
                                           response.body));
            return;
        }
        try {
            on_done(function_result::success(
                detail::json_to_value(nlohmann::json::parse(response.body))));
        } catch (const std::exception& e) {
            on_done(function_result::error(std::string("convex: unparsable upload response: ") +
                                           e.what()));
        }
    });
}

std::future<function_result> store_file(http_transport& transport, const std::string& upload_url,
                                        const std::string& content_type, bytes data) {
    auto promise = std::make_shared<std::promise<function_result>>();
    auto future = promise->get_future();
    store_file(transport, upload_url, content_type, std::move(data),
               [promise](function_result r) { promise->set_value(std::move(r)); });
    return future;
}

void fetch_file(http_transport& transport, const std::string& url,
                std::function<void(function_result)> on_done) {
    http_request request;
    request.method = "GET";
    request.url = url;

    transport.send(std::move(request), [on_done = std::move(on_done)](http_response response) {
        if (!response.error.empty()) {
            on_done(function_result::error("convex: download transport error: " + response.error));
            return;
        }
        if (response.status < 200 || response.status >= 300) {
            on_done(function_result::error("convex: download failed with HTTP " +
                                           std::to_string(response.status)));
            return;
        }
        on_done(function_result::success(
            value(bytes(response.body.begin(), response.body.end()))));
    });
}

std::future<function_result> fetch_file(http_transport& transport, const std::string& url) {
    auto promise = std::make_shared<std::promise<function_result>>();
    auto future = promise->get_future();
    fetch_file(transport, url,
               [promise](function_result r) { promise->set_value(std::move(r)); });
    return future;
}

}  // namespace convex
