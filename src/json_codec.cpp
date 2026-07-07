#include "detail/wire_json.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "detail/base64.h"

namespace convex::detail {

namespace {

using nlohmann::json;

// The wire format is little-endian; byte-swap on big-endian hosts.
template <typename T>
std::array<std::uint8_t, 8> to_le_bytes(T v) {
    static_assert(sizeof(T) == 8);
    std::array<std::uint8_t, 8> bytes;
    std::memcpy(bytes.data(), &v, 8);
    if constexpr (std::endian::native == std::endian::big) {
        std::reverse(bytes.begin(), bytes.end());
    }
    return bytes;
}

template <typename T>
T from_le_bytes(const std::vector<std::uint8_t>& bytes, const char* what) {
    static_assert(sizeof(T) == 8);
    if (bytes.size() != 8) {
        throw codec_error(std::string(what) + ": expected 8 bytes, got " +
                          std::to_string(bytes.size()));
    }
    std::array<std::uint8_t, 8> le;
    std::memcpy(le.data(), bytes.data(), 8);
    if constexpr (std::endian::native == std::endian::big) {
        std::reverse(le.begin(), le.end());
    }
    T v;
    std::memcpy(&v, le.data(), 8);
    return v;
}

std::vector<std::uint8_t> decode_b64(std::string_view b64, const char* what) {
    try {
        return base64_decode(b64);
    } catch (const std::invalid_argument& e) {
        throw codec_error(std::string(what) + ": " + e.what());
    }
}

// $float is reserved for doubles that plain JSON cannot represent: NaN,
// +/-Infinity and negative zero. Everything else is a bare number.
bool is_special_float(double d) {
    return !std::isfinite(d) || (d == 0.0 && std::signbit(d));
}

}  // namespace

std::string int64_to_le_base64(std::int64_t n) {
    const auto bytes = to_le_bytes(n);
    return base64_encode(bytes.data(), bytes.size());
}

std::string uint64_to_le_base64(std::uint64_t n) {
    const auto bytes = to_le_bytes(n);
    return base64_encode(bytes.data(), bytes.size());
}

std::string double_to_le_base64(double d) {
    const auto bytes = to_le_bytes(d);
    return base64_encode(bytes.data(), bytes.size());
}

std::int64_t le_base64_to_int64(std::string_view b64) {
    return from_le_bytes<std::int64_t>(decode_b64(b64, "$integer"), "$integer");
}

std::uint64_t le_base64_to_uint64(std::string_view b64) {
    return from_le_bytes<std::uint64_t>(decode_b64(b64, "timestamp"), "timestamp");
}

double le_base64_to_double(std::string_view b64) {
    return from_le_bytes<double>(decode_b64(b64, "$float"), "$float");
}

void validate_field_name(const std::string& name) {
    if (name.empty()) throw codec_error("field name must not be empty");
    if (name.size() > 1024) {
        throw codec_error("field name \"" + name.substr(0, 64) +
                          "...\" exceeds maximum field name length of 1024");
    }
    if (name[0] == '$') {
        throw codec_error("field name \"" + name + "\" starts with '$', which is reserved");
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 32 || u > 126) {
            throw codec_error("field name \"" + name + "\" has an invalid character: field names "
                              "must only contain non-control ASCII characters");
        }
    }
}

nlohmann::json value_to_json(const value& v) {
    switch (v.get_kind()) {
        case value::kind::null:
            return nullptr;
        case value::kind::boolean:
            return v.as_boolean();
        case value::kind::int64:
            return json{{"$integer", int64_to_le_base64(v.as_int64())}};
        case value::kind::float64: {
            const double d = v.as_float64();
            if (is_special_float(d)) return json{{"$float", double_to_le_base64(d)}};
            return d;
        }
        case value::kind::string:
            return v.as_string();
        case value::kind::bytes:
            return json{{"$bytes", base64_encode(v.as_bytes())}};
        case value::kind::array: {
            json arr = json::array();
            for (const value& element : v.as_array()) arr.push_back(value_to_json(element));
            return arr;
        }
        case value::kind::object: {
            json obj = json::object();
            for (const auto& [name, field] : v.as_object()) {
                validate_field_name(name);
                obj[name] = value_to_json(field);
            }
            return obj;
        }
    }
    throw codec_error("unreachable: unknown value kind");
}

value json_to_value(const nlohmann::json& j) {
    switch (j.type()) {
        case json::value_t::null:
            return value();
        case json::value_t::boolean:
            return value(j.get<bool>());
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float:
            // Bare JSON numbers are always Float64; Int64 arrives as $integer.
            return value(j.get<double>());
        case json::value_t::string:
            return value(j.get<std::string>());
        case json::value_t::array: {
            value_array arr;
            arr.reserve(j.size());
            for (const auto& element : j) arr.push_back(json_to_value(element));
            return value(std::move(arr));
        }
        case json::value_t::object: {
            if (j.size() == 1) {
                const auto it = j.begin();
                const std::string& key = it.key();
                if (key == "$integer") {
                    if (!it->is_string()) throw codec_error("$integer payload must be a string");
                    return value(le_base64_to_int64(it->get_ref<const std::string&>()));
                }
                if (key == "$float") {
                    if (!it->is_string()) throw codec_error("$float payload must be a string");
                    const double d = le_base64_to_double(it->get_ref<const std::string&>());
                    // Doubles that fit a plain JSON number must be sent bare;
                    // only -0.0 is exempt (it would decode as +0.0).
                    if (std::isfinite(d) && !(d == 0.0 && std::signbit(d))) {
                        throw codec_error("$float must not encode a plain-representable number");
                    }
                    return value(d);
                }
                if (key == "$bytes") {
                    if (!it->is_string()) throw codec_error("$bytes payload must be a string");
                    return value(decode_b64(it->get_ref<const std::string&>(), "$bytes"));
                }
                if (key == "$set" || key == "$map") {
                    throw codec_error(key + " is no longer supported by the Convex protocol");
                }
            }
            value_object obj;
            for (const auto& [name, field] : j.items()) {
                obj.emplace(name, json_to_value(field));
            }
            return value(std::move(obj));
        }
        default:
            throw codec_error("unsupported JSON type in Convex value");
    }
}

}  // namespace convex::detail

namespace convex {

std::string to_wire_json(const value& v) {
    return detail::value_to_json(v).dump();
}

value from_wire_json(std::string_view json_text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::exception& e) {
        throw codec_error(std::string("invalid JSON: ") + e.what());
    }
    return detail::json_to_value(j);
}

}  // namespace convex
