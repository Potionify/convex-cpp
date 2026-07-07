#pragma once

// Internal bridge between convex::value and nlohmann::json. Kept out of the
// public headers so nlohmann stays a private dependency of the library.

#include <convex/json_codec.h>
#include <convex/value.h>
#include <nlohmann/json.hpp>

namespace convex::detail {

/// value -> wire JSON DOM. Throws convex::codec_error (e.g. bad field name).
nlohmann::json value_to_json(const value& v);

/// wire JSON DOM -> value. Throws convex::codec_error.
value json_to_value(const nlohmann::json& j);

/// Throws codec_error unless `name` is a valid Convex field name:
/// non-empty, <= 1024 chars, printable ASCII (32..126), no leading '$'.
void validate_field_name(const std::string& name);

/// 8 little-endian bytes <-> 64-bit values, used by $integer/$float and
/// protocol timestamps.
std::string int64_to_le_base64(std::int64_t n);
std::string uint64_to_le_base64(std::uint64_t n);
std::string double_to_le_base64(double d);
std::int64_t le_base64_to_int64(std::string_view b64);
std::uint64_t le_base64_to_uint64(std::string_view b64);
double le_base64_to_double(std::string_view b64);

}  // namespace convex::detail
