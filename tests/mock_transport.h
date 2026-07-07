#pragma once

// In-memory websocket transport for driving convex::client in tests.
// The test controls each connection attempt: open it, feed it server
// messages, close it, and inspect the frames the client sent.

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <convex/transport.h>

namespace convex::testing {

class mock_transport final : public websocket_transport,
                             public std::enable_shared_from_this<mock_transport> {
public:
    struct attempt {
        std::string url;
        std::map<std::string, std::string> headers;
        websocket_observer* observer = nullptr;
        std::vector<std::string> sent;
        bool alive = true;  // false once the client destroyed the connection
    };

    std::unique_ptr<websocket_connection> connect(const std::string& url,
                                                  const std::map<std::string, std::string>& headers,
                                                  websocket_observer& observer) override {
        std::lock_guard lk(mu_);
        const std::size_t index = attempts_.size();
        attempts_.push_back(std::make_shared<attempt>());
        attempts_.back()->url = url;
        attempts_.back()->headers = headers;
        attempts_.back()->observer = &observer;
        cv_.notify_all();
        return std::make_unique<mock_connection>(shared_from_this(), index);
    }

    /// Block until `count` connection attempts have been made.
    bool wait_for_attempts(std::size_t count,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return attempts_.size() >= count; });
    }

    std::size_t attempt_count() {
        std::lock_guard lk(mu_);
        return attempts_.size();
    }

    void open(std::size_t index) { observer_of(index)->on_open(); }
    void server_send(std::size_t index, const std::string& text) {
        observer_of(index)->on_message(text);
    }
    void server_close(std::size_t index, const std::string& reason) {
        observer_of(index)->on_close(reason);
    }

    /// Frames sent by the client on attempt `index` so far.
    std::vector<std::string> sent(std::size_t index) {
        std::lock_guard lk(mu_);
        return attempts_.at(index)->sent;
    }

    /// Block until attempt `index` has sent at least `count` frames.
    bool wait_for_sent(std::size_t index, std::size_t count,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lk(mu_);
        return cv_.wait_for(lk, timeout,
                            [&] { return attempts_.at(index)->sent.size() >= count; });
    }

    bool connection_alive(std::size_t index) {
        std::lock_guard lk(mu_);
        return attempts_.at(index)->alive;
    }

private:
    class mock_connection final : public websocket_connection {
    public:
        mock_connection(std::shared_ptr<mock_transport> t, std::size_t index)
            : transport_(std::move(t)), index_(index) {}
        ~mock_connection() override {
            std::lock_guard lk(transport_->mu_);
            transport_->attempts_.at(index_)->alive = false;
            transport_->cv_.notify_all();
        }
        void send_text(std::string text) override {
            std::lock_guard lk(transport_->mu_);
            transport_->attempts_.at(index_)->sent.push_back(std::move(text));
            transport_->cv_.notify_all();
        }

    private:
        std::shared_ptr<mock_transport> transport_;
        std::size_t index_;
    };

    websocket_observer* observer_of(std::size_t index) {
        std::lock_guard lk(mu_);
        return attempts_.at(index)->observer;
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<attempt>> attempts_;
};

}  // namespace convex::testing
