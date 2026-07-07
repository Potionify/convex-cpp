#pragma once

// Default desktop transport built on IXWebSocket (built when the CMake
// option CONVEX_WITH_IXWEBSOCKET is ON). Engines with their own networking
// (e.g. Unreal) implement convex::websocket_transport directly instead.

#include <convex/transport.h>

#include <memory>

namespace convex::transports {

/// Create the IXWebSocket-backed websocket transport. Initializes the
/// platform network stack (WSAStartup on Windows) on first use.
std::shared_ptr<websocket_transport> make_ixwebsocket_transport();

}  // namespace convex::transports
