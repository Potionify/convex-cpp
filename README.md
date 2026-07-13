# convex-cpp

A C++20 client for [Convex](https://convex.dev), ported from the official
[convex-rs](https://github.com/get-convex/convex-rs) client with wire-protocol
details cross-checked against [convex-js](https://github.com/get-convex/convex-js).

- **Realtime sync client** (`convex::client`): WebSocket sync protocol with
  live query subscriptions, ordered mutations (read-your-writes), actions,
  authentication, and automatic reconnection with jittered backoff.
- **Pagination helper** (`convex::paginated_query`): a growing, live-updating
  list over a paginated query — the convex-js `usePaginatedQuery` state
  machine (one live subscription per page, `load_more`, seam-free page
  boundaries via query journals, resets on argument changes and stale
  cursors).
- **HTTP client** (`convex::http_client`): one-shot queries/mutations/actions
  over plain HTTP, plus file storage upload/download helpers.
- **Sans-IO core**: the protocol state machine (`convex::base_client`) does no
  networking. Transports plug in behind two small interfaces, so the library
  runs anywhere — a bundled [IXWebSocket](https://github.com/machinezone/IXWebSocket)
  transport serves desktop apps, and game engines supply their own (see
  [convex-ue](https://github.com/Potionify/convex-ue) for Unreal Engine).
- **Full Convex value fidelity**: Int64 (`$integer`), Bytes (`$bytes`),
  special floats (`$float` for NaN/±Infinity/-0.0), unicode strings, nested
  arrays/objects — byte-exact against the real backend, covered by
  conformance and live integration tests.

## Building

Requirements: CMake ≥ 3.21 and a C++20 compiler (MSVC 2022, clang, gcc).

```bash
cmake -S . -B build -DCONVEX_WITH_IXWEBSOCKET=ON
cmake --build build --config Release
```

| CMake option | Default | Effect |
|---|---|---|
| `CONVEX_BUILD_TESTS` | ON (top-level) | Unit tests (GoogleTest via FetchContent) |
| `CONVEX_WITH_IXWEBSOCKET` | OFF | Default desktop transport (fetches IXWebSocket + MbedTLS) |
| `CONVEX_BUILD_EXAMPLES` | OFF | `examples/chat_demo` |
| `CONVEX_BUILD_INTEGRATION_TESTS` | OFF | Live tests (need a running backend, see below) |

The core library target `convex::convex` has **no dependencies beyond the
vendored [nlohmann/json](third_party/nlohmann/json.hpp)** (a private,
implementation-only dependency — no public header includes it).

## Quick start

```cpp
#include <convex/convex.h>
#include "ixwebsocket/ixwebsocket_transport.h"

convex::client_options options;
options.deployment_url = "https://your-deployment.convex.cloud";
options.websocket = convex::transports::make_ixwebsocket_transport();
convex::client client(std::move(options));

// Live subscription: fires on every change.
auto sub = client.subscribe("messages:list",
    {{"channel", convex::value("general")}},
    [](const convex::function_result& r) {
        if (r.ok()) { /* r.get_value() is a convex::value */ }
    });

// Ordered mutation; the future resolves only after the subscription above
// already reflects the write (read-your-writes).
auto done = client.mutation("messages:send", {
    {"channel", convex::value("general")},
    {"body",    convex::value("hello from C++")},
});

// Pump callbacks on your main/game thread (default "pumped" delivery):
while (running) {
    client.process_events();
    // ... frame ...
}
```

Key types:

- `convex::value` — null / boolean / **int64** / float64 / string / **bytes** /
  array / object. `convex::value(42)` is an Int64; `convex::value(42.0)` a
  Float64 — Convex treats these as distinct types.
- `convex::function_result` — success value, plain error message, or a
  `convex_error` with the structured data payload of a thrown `ConvexError`.
- `client_options::delivery_mode` — `pumped` (callbacks fire inside
  `process_events()`, ideal for frame loops) or `immediate` (callbacks fire on
  internal threads).

Auth: `client.set_auth(convex::auth_token::user(jwt), fetcher)` — the optional
fetcher is invoked with `force_refresh=true` on every reconnect so expired
tokens are replaced. Admin: `convex::auth_token::admin(deploy_key)`.

### HTTP one-shot + file storage

```cpp
convex::http_client http("https://your-deployment.convex.cloud",
                         convex::transports::make_ixwebsocket_http_transport());
auto result = http.query("messages:list", {{"channel", convex::value("general")}}).get();

// Files: generateUploadUrl mutation -> POST bytes -> storageId.
auto url = http.mutation("files:generateUploadUrl", {}).get();
auto stored = convex::store_file(http.transport(), url.get_value().as_string(),
                                 "application/octet-stream", data).get();
```

## Custom transports

Implement `convex::websocket_transport` (plus `websocket_connection` /
`websocket_observer`) and/or `convex::http_transport` from
[`include/convex/transport.h`](include/convex/transport.h) and pass them in
the options. Rules of the contract:

- The client owns reconnect policy — transports must not auto-reconnect.
- `on_close` is terminal per connection; destroying a connection object must
  guarantee no further callbacks.

## Testing

```bash
# Unit tests (no network):
ctest --test-dir build -C Debug

# Live integration tests need the local open-source backend:
cd integration/backend && docker compose up -d
cd ../convex-test-project && npm install
CONVEX_SELF_HOSTED_URL=http://127.0.0.1:3210 \
CONVEX_SELF_HOSTED_ADMIN_KEY=<see integration/local.env> npx convex deploy -y
# then:
build/integration/tests/Debug/convex_integration_tests.exe
```

See [integration/README.md](integration/README.md) for details (cloud
deployment, admin key generation).

## Layout

```
include/convex/    public headers (value, protocol, base_client, client,
                   http_client, file_storage, transport)
src/               implementation (+ src/detail: base64, nlohmann bridge)
transports/        bundled IXWebSocket transport (optional)
tests/             unit tests: codec conformance, protocol shapes,
                   state-machine ordering, runtime client vs mock transport
integration/       dockerized convex-backend, test schema, live tests
```

## Protocol notes

The sync protocol implementation follows convex-rs's `BaseConvexClient`
(`base_client` here is its direct port) with wire shapes from convex-js
`src/browser/sync/protocol.ts`. Invariants worth knowing:

- Mutation results are held until a `Transition` advances past the mutation's
  timestamp, so completed mutations are always visible in query results.
- In-flight mutations are resent on reconnect (deduplicated server-side by
  session + request id); in-flight **actions fail** instead — they are not
  idempotent. Never-sent actions are resent.
- Query identity is the canonicalized `path + args` JSON with sorted object
  keys; identical subscriptions share one server query.
- Timestamps are opaque 64-bit tokens (base64, little-endian) — never doubles.

## License

Apache-2.0. Not an official Convex product.
