#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace convex {

class value;

/// Raw binary payload (Convex `Bytes`).
using bytes = std::vector<std::uint8_t>;

/// Ordered list of values (Convex `Array`).
using value_array = std::vector<value>;

/// Field map of a Convex `Object`. Keys are kept sorted (std::map), matching
/// convex-rs's BTreeMap. Sorted keys make every encoding of the same logical
/// object identical, which query-identity tokens rely on.
using value_object = std::map<std::string, value>;

/// Thrown by the typed as_*() accessors when the stored kind does not match.
class type_error : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// A Convex value: one of null, boolean, int64, float64, string, bytes,
/// array, or object. Mirrors convex-rs's `Value` enum (Set/Map/Id were
/// removed from the protocol and are not representable).
class value {
public:
    enum class kind : std::uint8_t {
        null,
        boolean,
        int64,
        float64,
        string,
        bytes,
        array,
        object,
    };

    value() noexcept : data_(nullptr) {}
    value(std::nullptr_t) noexcept : data_(nullptr) {}
    value(bool b) : data_(b) {}
    value(std::int32_t i) : data_(static_cast<std::int64_t>(i)) {}
    value(std::uint32_t i) : data_(static_cast<std::int64_t>(i)) {}
    value(std::int64_t i) : data_(i) {}
    value(double d) : data_(d) {}
    value(const char* s) : data_(std::string(s)) {}
    value(std::string_view s) : data_(std::string(s)) {}
    value(std::string s) : data_(std::move(s)) {}
    value(convex::bytes b) : data_(std::move(b)) {}
    value(value_array a) : data_(std::move(a)) {}
    value(value_object o) : data_(std::move(o)) {}

    kind get_kind() const noexcept { return static_cast<kind>(data_.index()); }

    bool is_null() const noexcept { return get_kind() == kind::null; }
    bool is_boolean() const noexcept { return get_kind() == kind::boolean; }
    bool is_int64() const noexcept { return get_kind() == kind::int64; }
    bool is_float64() const noexcept { return get_kind() == kind::float64; }
    bool is_string() const noexcept { return get_kind() == kind::string; }
    bool is_bytes() const noexcept { return get_kind() == kind::bytes; }
    bool is_array() const noexcept { return get_kind() == kind::array; }
    bool is_object() const noexcept { return get_kind() == kind::object; }

    bool as_boolean() const { return get<bool>("boolean"); }
    std::int64_t as_int64() const { return get<std::int64_t>("int64"); }
    double as_float64() const { return get<double>("float64"); }
    const std::string& as_string() const { return get<std::string>("string"); }
    const convex::bytes& as_bytes() const { return get<convex::bytes>("bytes"); }
    const value_array& as_array() const { return get<value_array>("array"); }
    const value_object& as_object() const { return get<value_object>("object"); }
    value_array& as_array() { return get<value_array>("array"); }
    value_object& as_object() { return get<value_object>("object"); }

    friend bool operator==(const value& a, const value& b) { return a.data_ == b.data_; }
    friend bool operator!=(const value& a, const value& b) { return !(a == b); }

    /// Human-readable kind name, for error messages.
    static std::string_view kind_name(kind k) noexcept;

private:
    template <typename T>
    const T& get(const char* wanted) const {
        if (const T* p = std::get_if<T>(&data_)) return *p;
        throw_mismatch(wanted);
    }
    template <typename T>
    T& get(const char* wanted) {
        if (T* p = std::get_if<T>(&data_)) return *p;
        throw_mismatch(wanted);
    }
    [[noreturn]] void throw_mismatch(const char* wanted) const;

    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                 convex::bytes, value_array, value_object>
        data_;
};

}  // namespace convex
