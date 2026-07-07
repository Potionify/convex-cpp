#include <convex/value.h>

namespace convex {

std::string_view value::kind_name(kind k) noexcept {
    switch (k) {
        case kind::null: return "null";
        case kind::boolean: return "boolean";
        case kind::int64: return "int64";
        case kind::float64: return "float64";
        case kind::string: return "string";
        case kind::bytes: return "bytes";
        case kind::array: return "array";
        case kind::object: return "object";
    }
    return "unknown";
}

void value::throw_mismatch(const char* wanted) const {
    throw type_error(std::string("convex::value: expected ") + wanted + " but holds " +
                     std::string(kind_name(get_kind())));
}

}  // namespace convex
