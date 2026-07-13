// Conformance tests for the Convex value type and wire JSON codec.
// Expected encodings mirror convex-rs (src/value/json) and convex-js
// (src/values/value.ts): $integer/$float/$bytes wrap base64 of 8
// little-endian bytes; $float only for NaN, +/-Infinity and -0.0.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>

#include <convex/json_codec.h>
#include <convex/value.h>
#include <gtest/gtest.h>

#include "detail/base64.h"

using convex::bytes;
using convex::codec_error;
using convex::from_wire_json;
using convex::to_wire_json;
using convex::value;
using convex::value_array;
using convex::value_object;

// ---------------------------------------------------------------- base64

TEST(Base64, KnownVectors) {
    const bytes empty{};
    EXPECT_EQ(convex::detail::base64_encode(empty), "");
    const bytes f{'f'};
    EXPECT_EQ(convex::detail::base64_encode(f), "Zg==");
    const bytes foob{'f', 'o', 'o', 'b'};
    EXPECT_EQ(convex::detail::base64_encode(foob), "Zm9vYg==");
    const bytes foobar{'f', 'o', 'o', 'b', 'a', 'r'};
    EXPECT_EQ(convex::detail::base64_encode(foobar), "Zm9vYmFy");

    EXPECT_EQ(convex::detail::base64_decode("Zm9vYmFy"), foobar);
    EXPECT_EQ(convex::detail::base64_decode("Zg=="), f);
    EXPECT_EQ(convex::detail::base64_decode(""), empty);

    EXPECT_THROW(convex::detail::base64_decode("Zg="), std::invalid_argument);   // bad length
    EXPECT_THROW(convex::detail::base64_decode("Zg=a"), std::invalid_argument);  // pad inside
    EXPECT_THROW(convex::detail::base64_decode("Z!=="), std::invalid_argument);  // bad char
}

// ------------------------------------------------------------- encoding

TEST(WireEncode, Primitives) {
    EXPECT_EQ(to_wire_json(value()), "null");
    EXPECT_EQ(to_wire_json(value(nullptr)), "null");
    EXPECT_EQ(to_wire_json(value(true)), "true");
    EXPECT_EQ(to_wire_json(value(false)), "false");
    EXPECT_EQ(to_wire_json(value("hello")), "\"hello\"");
    EXPECT_EQ(to_wire_json(value(1.5)), "1.5");
}

TEST(WireEncode, Int64) {
    EXPECT_EQ(to_wire_json(value(std::int64_t{0})), R"({"$integer":"AAAAAAAAAAA="})");
    EXPECT_EQ(to_wire_json(value(std::int64_t{1})), R"({"$integer":"AQAAAAAAAAA="})");
    EXPECT_EQ(to_wire_json(value(std::int64_t{42})), R"({"$integer":"KgAAAAAAAAA="})");
    EXPECT_EQ(to_wire_json(value(std::numeric_limits<std::int64_t>::min())),
              R"({"$integer":"AAAAAAAAAIA="})");
    // int and uint32 promote to Int64, never Float64.
    EXPECT_EQ(to_wire_json(value(42)), R"({"$integer":"KgAAAAAAAAA="})");
    EXPECT_EQ(to_wire_json(value(std::uint32_t{7u})),
              to_wire_json(value(std::int64_t{7})));
}

TEST(WireEncode, NaNPayloadsAreCanonicalized) {
    // Computed NaNs on x86-64 carry the negative pattern 0xFFF8...; JS
    // engines emit the canonical quiet NaN 0x7FF8.... Every NaN must encode
    // to the same canonical bytes so query-identity tokens are deterministic.
    const double negative_nan = std::bit_cast<double>(std::uint64_t{0xFFF8000000000000ULL});
    const double payload_nan = std::bit_cast<double>(std::uint64_t{0x7FF0000000000001ULL});
    ASSERT_TRUE(std::isnan(negative_nan));
    ASSERT_TRUE(std::isnan(payload_nan));
    EXPECT_EQ(to_wire_json(value(negative_nan)), R"({"$float":"AAAAAAAA+H8="})");
    EXPECT_EQ(to_wire_json(value(payload_nan)), R"({"$float":"AAAAAAAA+H8="})");
}

TEST(WireEncode, SpecialFloats) {
    EXPECT_EQ(to_wire_json(value(std::numeric_limits<double>::quiet_NaN())),
              R"({"$float":"AAAAAAAA+H8="})");
    EXPECT_EQ(to_wire_json(value(std::numeric_limits<double>::infinity())),
              R"({"$float":"AAAAAAAA8H8="})");
    EXPECT_EQ(to_wire_json(value(-std::numeric_limits<double>::infinity())),
              R"({"$float":"AAAAAAAA8P8="})");
    EXPECT_EQ(to_wire_json(value(-0.0)), R"({"$float":"AAAAAAAAAIA="})");
    // Plain zero and ordinary doubles stay bare numbers.
    EXPECT_EQ(to_wire_json(value(0.0)), "0.0");
    EXPECT_EQ(to_wire_json(value(-2.5)), "-2.5");
}

TEST(WireEncode, Bytes) {
    EXPECT_EQ(to_wire_json(value(bytes{0, 1, 2, 3, 4, 5, 6, 7})),
              R"({"$bytes":"AAECAwQFBgc="})");
    EXPECT_EQ(to_wire_json(value(bytes{})), R"({"$bytes":""})");
}

TEST(WireEncode, Compound) {
    value doc(value_object{
        {"body", value("hi")},
        {"count", value(std::int64_t{3})},
        {"ratio", value(0.5)},
        {"tags", value(value_array{value("a"), value("b")})},
    });
    // std::map keeps keys sorted, so the encoding is canonical.
    EXPECT_EQ(to_wire_json(doc),
              R"({"body":"hi","count":{"$integer":"AwAAAAAAAAA="},"ratio":0.5,"tags":["a","b"]})");
}

TEST(WireEncode, FieldNameValidation) {
    EXPECT_THROW(to_wire_json(value(value_object{{"$reserved", value(1.0)}})), codec_error);
    EXPECT_THROW(to_wire_json(value(value_object{{"", value(1.0)}})), codec_error);
    EXPECT_THROW(to_wire_json(value(value_object{{std::string(1025, 'a'), value(1.0)}})),
                 codec_error);
    EXPECT_THROW(to_wire_json(value(value_object{{"tab\there", value(1.0)}})), codec_error);
    EXPECT_THROW(to_wire_json(value(value_object{{"h\xC3\xA9llo", value(1.0)}})), codec_error);
    // 1024 chars exactly is allowed; '_' prefix (system fields) is allowed.
    EXPECT_NO_THROW(to_wire_json(value(value_object{{std::string(1024, 'a'), value(1.0)}})));
    EXPECT_NO_THROW(to_wire_json(value(value_object{{"_creationTime", value(1.0)}})));
    // Validation applies to nested objects too.
    EXPECT_THROW(to_wire_json(value(value_array{value(value_object{{"$x", value(1.0)}})})),
                 codec_error);
}

// ------------------------------------------------------------- decoding

TEST(WireDecode, BareNumbersAreFloat64) {
    EXPECT_EQ(from_wire_json("42"), value(42.0));
    EXPECT_EQ(from_wire_json("-1.25"), value(-1.25));
    EXPECT_EQ(from_wire_json("1"), value(1.0));
    EXPECT_TRUE(from_wire_json("3.0").is_float64());
}

TEST(WireDecode, Int64) {
    EXPECT_EQ(from_wire_json(R"({"$integer":"AQAAAAAAAAA="})"), value(std::int64_t{1}));
    EXPECT_EQ(from_wire_json(R"({"$integer":"AAAAAAAAAIA="})"),
              value(std::numeric_limits<std::int64_t>::min()));
    EXPECT_THROW(from_wire_json(R"({"$integer":"AAAA"})"), codec_error);   // not 8 bytes
    EXPECT_THROW(from_wire_json(R"({"$integer":42})"), codec_error);       // not a string
    EXPECT_THROW(from_wire_json(R"({"$integer":"!!!!"})"), codec_error);   // bad base64
}

TEST(WireDecode, SpecialFloats) {
    const value nan_v = from_wire_json(R"({"$float":"AAAAAAAA+H8="})");
    ASSERT_TRUE(nan_v.is_float64());
    EXPECT_TRUE(std::isnan(nan_v.as_float64()));

    EXPECT_EQ(from_wire_json(R"({"$float":"AAAAAAAA8H8="})").as_float64(),
              std::numeric_limits<double>::infinity());
    EXPECT_EQ(from_wire_json(R"({"$float":"AAAAAAAA8P8="})").as_float64(),
              -std::numeric_limits<double>::infinity());

    const value neg_zero = from_wire_json(R"({"$float":"AAAAAAAAAIA="})");
    ASSERT_TRUE(neg_zero.is_float64());
    EXPECT_TRUE(std::signbit(neg_zero.as_float64()));
    EXPECT_EQ(neg_zero.as_float64(), 0.0);

    // A $float that plain JSON could represent is a protocol violation
    // (1.5 == 0x3FF8000000000000, little-endian base64 below).
    EXPECT_THROW(from_wire_json(R"({"$float":"AAAAAAAA+D8="})"), codec_error);
}

TEST(WireDecode, Bytes) {
    EXPECT_EQ(from_wire_json(R"({"$bytes":"AAECAwQFBgc="})"),
              value(bytes{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(from_wire_json(R"({"$bytes":""})"), value(bytes{}));
}

TEST(WireDecode, LegacyWrappersRejected) {
    EXPECT_THROW(from_wire_json(R"({"$set":[1,2]})"), codec_error);
    EXPECT_THROW(from_wire_json(R"({"$map":[[1,2]]})"), codec_error);
}

TEST(WireDecode, UnknownDollarKeysDecodeAsPlainObjects) {
    // Matches convex-rs: only the known wrappers are special; any other
    // single-key object decodes as a plain Object.
    const value v = from_wire_json(R"({"$unknown":1})");
    ASSERT_TRUE(v.is_object());
    EXPECT_EQ(v.as_object().at("$unknown"), value(1.0));
}

TEST(WireDecode, SystemFields) {
    const value doc = from_wire_json(
        R"({"_creationTime":1720000000000.5,"_id":"j57w3","body":"hi"})");
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc.as_object().at("_id"), value("j57w3"));
}

TEST(WireDecode, MalformedJson) {
    EXPECT_THROW(from_wire_json("{"), codec_error);
    EXPECT_THROW(from_wire_json(""), codec_error);
    EXPECT_THROW(from_wire_json("{'single':1}"), codec_error);
}

// ------------------------------------------------------------ round-trip

namespace {

value random_value(std::mt19937_64& rng, int depth) {
    std::uniform_int_distribution<int> pick(0, depth > 0 ? 7 : 5);
    switch (pick(rng)) {
        case 0: return value();
        case 1: return value(rng() % 2 == 0);
        case 2: return value(static_cast<std::int64_t>(rng()));
        case 3: {
            // Finite doubles only; NaN breaks operator== round-trip checks
            // and is covered by a dedicated test.
            std::uniform_real_distribution<double> d(-1e18, 1e18);
            return value(d(rng));
        }
        case 4: {
            std::string s;
            const auto len = rng() % 20;
            for (std::size_t i = 0; i < len; ++i) s.push_back(static_cast<char>('a' + rng() % 26));
            return value(std::move(s));
        }
        case 5: {
            bytes b;
            const auto len = rng() % 20;
            for (std::size_t i = 0; i < len; ++i) b.push_back(static_cast<std::uint8_t>(rng()));
            return value(std::move(b));
        }
        case 6: {
            value_array arr;
            const auto len = rng() % 5;
            for (std::size_t i = 0; i < len; ++i) arr.push_back(random_value(rng, depth - 1));
            return value(std::move(arr));
        }
        default: {
            value_object obj;
            const auto len = rng() % 5;
            for (std::size_t i = 0; i < len; ++i) {
                std::string k = "k" + std::to_string(rng() % 100);
                obj[std::move(k)] = random_value(rng, depth - 1);
            }
            return value(std::move(obj));
        }
    }
}

}  // namespace

TEST(RoundTrip, RandomizedDeterministic) {
    std::mt19937_64 rng(20260707);
    for (int i = 0; i < 500; ++i) {
        const value original = random_value(rng, 3);
        const value decoded = from_wire_json(to_wire_json(original));
        EXPECT_EQ(original, decoded) << "iteration " << i << ": " << to_wire_json(original);
    }
}

TEST(RoundTrip, UnicodeStrings) {
    const value v(std::string("h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x8E\xAE"));
    EXPECT_EQ(from_wire_json(to_wire_json(v)), v);
}

TEST(RoundTrip, SpecialFloatsBitExact) {
    for (const double d : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(), -0.0}) {
        const value decoded = from_wire_json(to_wire_json(value(d)));
        ASSERT_TRUE(decoded.is_float64());
        std::uint64_t in_bits, out_bits;
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        std::memcpy(&in_bits, &d, 8);
        const double out = decoded.as_float64();
        std::memcpy(&out_bits, &out, 8);
        EXPECT_EQ(in_bits, out_bits);
    }
}

TEST(RoundTrip, Int64Extremes) {
    for (const std::int64_t n : {std::numeric_limits<std::int64_t>::min(),
                                 std::numeric_limits<std::int64_t>::max(), std::int64_t{0},
                                 std::int64_t{-1}, std::int64_t{9007199254740993}}) {
        EXPECT_EQ(from_wire_json(to_wire_json(value(n))), value(n));
    }
}

// ----------------------------------------------------------- value type

TEST(Value, KindsAndAccessors) {
    EXPECT_TRUE(value().is_null());
    EXPECT_EQ(value(true).as_boolean(), true);
    EXPECT_EQ(value(std::int64_t{5}).as_int64(), 5);
    EXPECT_EQ(value(2.5).as_float64(), 2.5);
    EXPECT_EQ(value("s").as_string(), "s");
    EXPECT_THROW(value(2.5).as_string(), convex::type_error);
    EXPECT_THROW(value("s").as_int64(), convex::type_error);
}

TEST(Value, IntIsNotFloat) {
    EXPECT_NE(value(std::int64_t{1}), value(1.0));
    EXPECT_TRUE(value(1).is_int64());
    EXPECT_TRUE(value(1.0).is_float64());
}
