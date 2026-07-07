#pragma once

#include <convex/value.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace convex {

/// Thrown when a value cannot be encoded to, or decoded from, the Convex
/// wire JSON format.
class codec_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Encode a value into Convex wire JSON:
///   - Int64            -> {"$integer": "<base64 of 8 little-endian bytes>"}
///   - Float64 special  -> {"$float":   "<base64 of 8 little-endian bytes>"}
///     (only NaN, +/-Infinity and -0.0; all other doubles are bare numbers)
///   - Bytes            -> {"$bytes":   "<base64>"}
/// Object field names are validated (<= 1024 chars, no leading '$',
/// printable ASCII 32..126 only); violations throw codec_error.
std::string to_wire_json(const value& v);

/// Decode Convex wire JSON produced by a Convex backend. Bare JSON numbers
/// decode as Float64. Legacy "$set"/"$map" wrappers are rejected.
/// Throws codec_error on malformed input.
value from_wire_json(std::string_view json_text);

}  // namespace convex
