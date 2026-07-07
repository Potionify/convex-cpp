#pragma once

#include <convex/value.h>

#include <string>
#include <variant>

namespace convex {

/// An application error thrown by a Convex function via `ConvexError`,
/// carrying a developer-defined data payload.
struct convex_error {
    std::string message;
    value data;

    friend bool operator==(const convex_error& a, const convex_error& b) {
        return a.message == b.message && a.data == b.data;
    }
};

/// The outcome of running a Convex query, mutation, or action. Mirrors
/// convex-rs's FunctionResult: function-level failures are carried in the
/// result (not thrown), so subscription updates can deliver errors as data.
class function_result {
public:
    /// Successful run producing a value.
    static function_result success(value v) { return function_result(std::move(v)); }
    /// Developer/system error with only a message (redacted in prod).
    static function_result error(std::string message) {
        return function_result(error_tag{}, std::move(message));
    }
    /// Application error raised via ConvexError, with data payload.
    static function_result error(convex_error e) { return function_result(std::move(e)); }

    bool ok() const noexcept { return std::holds_alternative<value>(raw_); }
    bool is_app_error() const noexcept { return std::holds_alternative<convex_error>(raw_); }

    /// The success value. Throws type_error when !ok().
    const value& get_value() const {
        if (const value* v = std::get_if<value>(&raw_)) return *v;
        throw type_error("function_result: not a success: " + error_message());
    }

    /// The error message, for either error flavor. Empty when ok().
    std::string error_message() const {
        if (const std::string* m = std::get_if<std::string>(&raw_)) return *m;
        if (const convex_error* e = std::get_if<convex_error>(&raw_)) return e->message;
        return {};
    }

    /// The ConvexError payload, or nullptr when this is not an app error.
    const convex_error* app_error() const noexcept { return std::get_if<convex_error>(&raw_); }

    friend bool operator==(const function_result& a, const function_result& b) {
        return a.raw_ == b.raw_;
    }

private:
    struct error_tag {};
    explicit function_result(value v) : raw_(std::move(v)) {}
    function_result(error_tag, std::string m) : raw_(std::move(m)) {}
    explicit function_result(convex_error e) : raw_(std::move(e)) {}

    std::variant<value, std::string, convex_error> raw_;
};

}  // namespace convex
