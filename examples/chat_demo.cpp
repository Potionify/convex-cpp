// Minimal realtime chat against the integration test schema
// (integration/convex-test-project). Start the local backend first:
//   cd integration/backend && docker compose up -d
//
// Usage: chat_demo [deployment_url] [channel] [author]

#include <cstdio>
#include <iostream>
#include <string>

#include <convex/convex.h>

#include "ixwebsocket/ixwebsocket_transport.h"

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:3210";
    const std::string channel = argc > 2 ? argv[2] : "demo";
    const std::string author = argc > 3 ? argv[3] : "cpp-demo";

    convex::client_options options;
    options.deployment_url = url;
    options.websocket = convex::transports::make_ixwebsocket_transport();
    // Immediate delivery: callbacks fire on the client's internal threads,
    // which is fine for a console app. A game would use pumped delivery and
    // call process_events() once per frame instead.
    options.delivery_mode = convex::client_options::delivery::immediate;
    convex::client client(std::move(options));

    client.on_state_change([](convex::connection_state s) {
        const char* name = s == convex::connection_state::connected      ? "connected"
                           : s == convex::connection_state::connecting   ? "connecting"
                                                                          : "disconnected";
        std::fprintf(stderr, "[connection: %s]\n", name);
    });

    auto subscription = client.subscribe(
        "messages:list", {{"channel", convex::value(channel)}},
        [](const convex::function_result& result) {
            if (!result.ok()) {
                std::fprintf(stderr, "[query error: %s]\n", result.error_message().c_str());
                return;
            }
            std::printf("\r--- %zu message(s) ---\n", result.get_value().as_array().size());
            for (const convex::value& message : result.get_value().as_array()) {
                const auto& doc = message.as_object();
                std::printf("%s: %s\n", doc.at("author").as_string().c_str(),
                            doc.at("body").as_string().c_str());
            }
            std::printf("> ");
            std::fflush(stdout);
        });

    std::printf("Connected to %s, channel '%s'. Type a message, or /quit.\n> ", url.c_str(),
                channel.c_str());
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "/quit") break;
        if (line.empty()) continue;
        client.mutation("messages:send",
                        {{"channel", convex::value(channel)},
                         {"author", convex::value(author)},
                         {"body", convex::value(line)}},
                        [](convex::function_result result) {
                            if (!result.ok()) {
                                std::fprintf(stderr, "[send failed: %s]\n",
                                             result.error_message().c_str());
                            }
                        });
    }
    return 0;
}
