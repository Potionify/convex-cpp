#pragma once

// Transport abstraction: convex-cpp contains no networking of its own.
// Standalone apps can use the bundled IXWebSocket implementation (CMake
// option CONVEX_WITH_IXWEBSOCKET); engines plug in their own (the Unreal
// plugin adapts UE's WebSockets and HTTP modules).

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace convex {

/// Receives events for one websocket connection. Implementations of
/// websocket_transport may invoke these from any thread, but never
/// concurrently for the same connection.
class websocket_observer {
public:
    virtual ~websocket_observer() = default;

    /// The connection is established and ready for send_text().
    virtual void on_open() = 0;

    /// A complete text frame arrived.
    virtual void on_message(std::string text) = 0;

    /// The connection closed or failed (including a failed connect attempt).
    /// Terminal: no further callbacks follow. `reason` feeds the sync
    /// protocol's lastCloseReason.
    virtual void on_close(std::string reason) = 0;
};

/// One websocket connection. Destroying the object must close the socket and
/// guarantee no further observer callbacks.
class websocket_connection {
public:
    virtual ~websocket_connection() = default;

    /// Queue a text frame. Called only between on_open() and on_close().
    virtual void send_text(std::string text) = 0;
};

/// Factory for websocket connections.
class websocket_transport {
public:
    virtual ~websocket_transport() = default;

    /// Begin connecting asynchronously; the observer (owned by the caller,
    /// outliving the connection) gets on_open() or on_close(). `headers` are
    /// extra HTTP headers for the upgrade request (e.g. Convex-Client).
    virtual std::unique_ptr<websocket_connection> connect(
        const std::string& url, const std::map<std::string, std::string>& headers,
        websocket_observer& observer) = 0;
};

/// An HTTP exchange, for the one-shot client and file storage.
struct http_request {
    std::string method = "POST";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct http_response {
    /// HTTP status, or 0 when the request failed before reaching the server
    /// (then `error` describes why).
    int status = 0;
    std::string body;
    std::string error;

    bool transport_ok() const { return error.empty() && status != 0; }
};

/// Asynchronous HTTP client factory-less interface.
class http_transport {
public:
    virtual ~http_transport() = default;

    /// Execute a request; invoke `on_done` exactly once, from any thread.
    virtual void send(http_request request,
                      std::function<void(http_response)> on_done) = 0;
};

}  // namespace convex
