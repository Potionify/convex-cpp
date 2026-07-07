#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace convex::detail {

/// Standard-alphabet base64 with padding (RFC 4648), as used by the Convex
/// wire format for $integer/$float/$bytes payloads and timestamps.
std::string base64_encode(const std::uint8_t* data, std::size_t len);
inline std::string base64_encode(const std::vector<std::uint8_t>& data) {
    return base64_encode(data.data(), data.size());
}

/// Throws std::invalid_argument on malformed input.
std::vector<std::uint8_t> base64_decode(std::string_view text);

}  // namespace convex::detail
