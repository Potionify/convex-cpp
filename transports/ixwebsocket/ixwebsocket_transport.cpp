#include "ixwebsocket_transport.h"

#include <atomic>
#include <mutex>

#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace convex::transports {

namespace {

void ensure_net_system() {
    static std::once_flag once;
    std::call_once(once, [] { ix::initNetSystem(); });
}

class ix_connection final : public websocket_connection {
public:
    ix_connection(const std::string& url, const std::map<std::string, std::string>& headers,
                  websocket_observer& observer)
        : observer_(observer) {
        ws_.setUrl(url);
        ix::WebSocketHttpHeaders extra;
        for (const auto& [k, v] : headers) extra[k] = v;
        ws_.setExtraHeaders(extra);
        // convex::client owns reconnection policy; a transport that silently
        // reconnects would corrupt the sync protocol (versions reset per
        // physical connection).
        ws_.disableAutomaticReconnection();
        ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    observer_.on_open();
                    break;
                case ix::WebSocketMessageType::Message:
                    if (!msg->binary) observer_.on_message(msg->str);
                    break;
                case ix::WebSocketMessageType::Error:
                    report_close(msg->errorInfo.reason.empty() ? "TransportError"
                                                               : msg->errorInfo.reason);
                    break;
                case ix::WebSocketMessageType::Close:
                    report_close(msg->closeInfo.reason.empty()
                                     ? "Closed(" + std::to_string(msg->closeInfo.code) + ")"
                                     : msg->closeInfo.reason);
                    break;
                default:
                    break;  // Ping/Pong/Fragment: nothing to surface
            }
        });
        ws_.start();
    }

    ~ix_connection() override {
        // stop() closes the socket and joins the background thread, so no
        // callback can be in flight once destruction completes.
        ws_.stop();
    }

    void send_text(std::string text) override { ws_.sendText(text); }

private:
    void report_close(std::string reason) {
        // IXWebSocket can emit Error followed by Close for one failure; the
        // observer contract says on_close is terminal, so forward only once.
        if (!close_reported_.exchange(true)) observer_.on_close(std::move(reason));
    }

    websocket_observer& observer_;
    std::atomic_bool close_reported_{false};
    ix::WebSocket ws_;
};

class ix_transport final : public websocket_transport {
public:
    std::unique_ptr<websocket_connection> connect(
        const std::string& url, const std::map<std::string, std::string>& headers,
        websocket_observer& observer) override {
        ensure_net_system();
        return std::make_unique<ix_connection>(url, headers, observer);
    }
};

class ix_http_transport final : public http_transport {
public:
    ix_http_transport() : client_(/*async=*/true) { ensure_net_system(); }

    void send(http_request request, std::function<void(http_response)> on_done) override {
        auto args = client_.createRequest(request.url, request.method);
        for (const auto& [k, v] : request.headers) args->extraHeaders[k] = v;
        args->body = std::move(request.body);
        const bool queued = client_.performRequest(
            args, [on_done = std::move(on_done)](const ix::HttpResponsePtr& r) {
                http_response out;
                if (r == nullptr) {
                    out.error = "no response";
                } else if (r->errorCode != ix::HttpErrorCode::Ok) {
                    out.error = r->errorMsg.empty() ? "http error" : r->errorMsg;
                    out.status = r->statusCode;
                    out.body = r->body;
                } else {
                    out.status = r->statusCode;
                    out.body = r->body;
                }
                on_done(std::move(out));
            });
        if (!queued) {
            http_response out;
            out.error = "failed to queue request";
            on_done(std::move(out));
        }
    }

private:
    ix::HttpClient client_;
};

}  // namespace

std::shared_ptr<websocket_transport> make_ixwebsocket_transport() {
    return std::make_shared<ix_transport>();
}

std::shared_ptr<http_transport> make_ixwebsocket_http_transport() {
    return std::make_shared<ix_http_transport>();
}

}  // namespace convex::transports
