#include "detail/base64.h"

#include <array>
#include <stdexcept>

namespace convex::detail {

namespace {
constexpr char k_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<std::int8_t, 256> make_reverse_table() {
    std::array<std::int8_t, 256> table{};
    for (auto& e : table) e = -1;
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(k_alphabet[i])] = static_cast<std::int8_t>(i);
    return table;
}
constexpr auto k_reverse = make_reverse_table();
}  // namespace

std::string base64_encode(const std::uint8_t* data, std::size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                          (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                          static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(k_alphabet[(n >> 18) & 63]);
        out.push_back(k_alphabet[(n >> 12) & 63]);
        out.push_back(k_alphabet[(n >> 6) & 63]);
        out.push_back(k_alphabet[n & 63]);
        i += 3;
    }
    const std::size_t rest = len - i;
    if (rest == 1) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(k_alphabet[(n >> 18) & 63]);
        out.push_back(k_alphabet[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                          (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(k_alphabet[(n >> 18) & 63]);
        out.push_back(k_alphabet[(n >> 12) & 63]);
        out.push_back(k_alphabet[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) throw std::invalid_argument("base64: length not a multiple of 4");
    std::size_t len = text.size();
    std::size_t padding = 0;
    while (padding < 2 && len > 0 && text[len - 1 - padding] == '=') ++padding;

    std::vector<std::uint8_t> out;
    out.reserve((len / 4) * 3);
    for (std::size_t i = 0; i < len; i += 4) {
        std::uint32_t n = 0;
        int valid = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                // Padding only allowed in the final quantum's tail.
                if (i + 4 != len || (j == 2 && text[i + 3] != '=')) {
                    throw std::invalid_argument("base64: unexpected padding");
                }
                n <<= 6;
                continue;
            }
            const std::int8_t d = k_reverse[static_cast<unsigned char>(c)];
            if (d < 0) throw std::invalid_argument("base64: invalid character");
            n = (n << 6) | static_cast<std::uint32_t>(d);
            valid = j + 1;
        }
        out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xff));
        if (valid >= 3) out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
        if (valid == 4) out.push_back(static_cast<std::uint8_t>(n & 0xff));
    }
    return out;
}

}  // namespace convex::detail
