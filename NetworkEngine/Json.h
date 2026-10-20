/// @file Json.h
/// @brief A small JSON reader and writer for the driver's config files.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AES67 {

/// One JSON value. Numbers keep their literal text, so a 64-bit integer
/// survives a round trip and range checks happen when a value is read, not
/// when it is parsed. Objects keep their members in file order.
///
/// The typed accessors never throw: a value of the wrong type, or a number
/// that doesn't fit, reads as nullopt (or nullptr).
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    struct Member;
    using Array = std::vector<JsonValue>;
    using Object = std::vector<Member>;

    /// Deeper nesting than this is refused rather than risking the stack.
    static constexpr int kMaxDepth = 64;

    JsonValue() = default;  // null

    static JsonValue boolean(bool value);
    static JsonValue number(uint64_t value);
    static JsonValue number(int64_t value);
    static JsonValue number(int value) { return number(static_cast<int64_t>(value)); }
    /// Non-finite values have no JSON form and become null.
    static JsonValue number(double value);
    static JsonValue string(std::string value);
    static JsonValue array(Array items = {});
    static JsonValue object(Object members = {});

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }

    std::optional<bool> asBool() const;
    /// An integer literal (no fraction or exponent) that fits.
    std::optional<uint64_t> asUInt64() const;
    std::optional<int64_t> asInt64() const;
    /// Any number that is finite as a double.
    std::optional<double> asDouble() const;
    const std::string* asString() const;
    const Array* asArray() const;
    const Object* asObject() const;

    /// An object's member by key (the last one, if repeated), else nullptr.
    const JsonValue* find(std::string_view key) const;

    /// Append to an array / add a member to an object.
    JsonValue& push(JsonValue item);
    JsonValue& add(std::string key, JsonValue value);

    /// Pretty-printed with `indent` spaces per level; arrays holding no
    /// arrays or objects stay on one line.
    std::string dump(int indent = 2) const;

    /// Strict JSON, with two allowances so one odd string can't lose a whole
    /// file: raw control characters inside strings are accepted (older
    /// streams.json files were written with them unescaped), and a \u escape
    /// naming half a surrogate pair reads as U+FFFD.
    /// nullopt on any error, with the byte offset and reason in `error`.
    static std::optional<JsonValue> parse(std::string_view text, std::string* error = nullptr);

private:
    friend class JsonParser;

    void dumpTo(std::string& out, int indent, int level) const;

    Type type_{Type::Null};
    bool bool_{false};
    std::string text_;  // a string's value, or a number's literal
    Array array_;
    Object object_;
};

struct JsonValue::Member {
    std::string key;
    JsonValue value;
};

} // namespace AES67
