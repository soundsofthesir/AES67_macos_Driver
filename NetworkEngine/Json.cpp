/// @file Json.cpp

#include "Json.h"
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <xlocale.h>

namespace AES67 {

namespace {

/// Number text is always read and written in the C locale: a host process
/// that set, say, a German LC_NUMERIC would otherwise write "0,125".
locale_t cLocale() {
    static locale_t locale = newlocale(LC_ALL_MASK, "C", nullptr);
    return locale;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

/// The JSON number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
size_t numberLength(std::string_view text) {
    size_t i = 0;
    if (i < text.size() && text[i] == '-') ++i;
    if (i >= text.size()) return 0;
    if (text[i] == '0') {
        ++i;
    } else if (isDigit(text[i])) {
        while (i < text.size() && isDigit(text[i])) ++i;
    } else {
        return 0;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        if (i >= text.size() || !isDigit(text[i])) return 0;
        while (i < text.size() && isDigit(text[i])) ++i;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        if (i >= text.size() || !isDigit(text[i])) return 0;
        while (i < text.size() && isDigit(text[i])) ++i;
    }
    return i;
}

bool isIntegerLiteral(const std::string& literal) {
    return literal.find_first_of(".eE") == std::string::npos;
}

void appendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint < 0x80) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        out += static_cast<char>(0xC0 | (codePoint >> 6));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codePoint >> 12));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codePoint >> 18));
        out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

void appendEscaped(std::string& out, const std::string& value) {
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    for (char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += c;  // UTF-8 passes through as bytes
                }
        }
    }
    out += '"';
}

} // namespace

// ============================================================================
// Parser
// ============================================================================

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    std::optional<JsonValue> parseDocument(std::string* error) {
        JsonValue value;
        skipSpace();
        if (parseValue(value, 0)) {
            skipSpace();
            if (pos_ == text_.size()) return value;
            fail("unexpected text after the value");
        }
        if (error) *error = "at byte " + std::to_string(errorPos_) + ": " + error_;
        return std::nullopt;
    }

private:
    bool fail(const char* reason) {
        if (error_.empty()) {
            error_ = reason;
            errorPos_ = pos_;
        }
        return false;
    }

    void skipSpace() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool take(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool takeWord(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > JsonValue::kMaxDepth) return fail("nested too deeply");
        if (pos_ >= text_.size()) return fail("unexpected end of input");
        switch (text_[pos_]) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"':
                out.type_ = JsonValue::Type::String;
                return parseString(out.text_);
            case 't':
            case 'f':
                out.type_ = JsonValue::Type::Bool;
                out.bool_ = text_[pos_] == 't';
                return takeWord(out.bool_ ? "true" : "false") || fail("invalid literal");
            case 'n':
                out.type_ = JsonValue::Type::Null;
                return takeWord("null") || fail("invalid literal");
            default: {
                const size_t length = numberLength(text_.substr(pos_));
                if (length == 0) return fail("expected a value");
                out.type_ = JsonValue::Type::Number;
                out.text_ = std::string(text_.substr(pos_, length));
                pos_ += length;
                return true;
            }
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        out.type_ = JsonValue::Type::Object;
        ++pos_;  // '{'
        skipSpace();
        if (take('}')) return true;
        for (;;) {
            skipSpace();
            JsonValue::Member member;
            if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected a quoted key");
            if (!parseString(member.key)) return false;
            skipSpace();
            if (!take(':')) return fail("expected ':'");
            skipSpace();
            if (!parseValue(member.value, depth + 1)) return false;
            out.object_.push_back(std::move(member));
            skipSpace();
            if (take('}')) return true;
            if (!take(',')) return fail("expected ',' or '}'");
        }
    }

    bool parseArray(JsonValue& out, int depth) {
        out.type_ = JsonValue::Type::Array;
        ++pos_;  // '['
        skipSpace();
        if (take(']')) return true;
        for (;;) {
            skipSpace();
            JsonValue item;
            if (!parseValue(item, depth + 1)) return false;
            out.array_.push_back(std::move(item));
            skipSpace();
            if (take(']')) return true;
            if (!take(',')) return fail("expected ',' or ']'");
        }
    }

    bool parseHex4(uint32_t& out) {
        if (text_.size() - pos_ < 4) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (isDigit(c)) out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    /// An unpaired surrogate reads as U+FFFD rather than failing the whole
    /// document: it is a bad character in one string, nothing more.
    bool parseUnicodeEscape(std::string& out) {
        constexpr uint32_t kReplacement = 0xFFFD;
        uint32_t codePoint = 0;
        if (!parseHex4(codePoint)) return false;
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            // A high surrogate needs its low half next
            const size_t afterHigh = pos_;
            uint32_t low = 0;
            if (takeWord("\\u") && parseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            } else {
                if (!error_.empty()) return false;  // a malformed escape after it
                pos_ = afterHigh;  // whatever follows is read on its own
                codePoint = kReplacement;
            }
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
            codePoint = kReplacement;
        }
        appendUtf8(out, codePoint);
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) break;
            switch (text_[pos_++]) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u':
                    if (!parseUnicodeEscape(out)) return false;
                    break;
                default:
                    --pos_;
                    return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    std::string_view text_;
    size_t pos_{0};
    std::string error_;
    size_t errorPos_{0};
};

std::optional<JsonValue> JsonValue::parse(std::string_view text, std::string* error) {
    return JsonParser(text).parseDocument(error);
}

// ============================================================================
// Construction and access
// ============================================================================

JsonValue JsonValue::boolean(bool value) {
    JsonValue v;
    v.type_ = Type::Bool;
    v.bool_ = value;
    return v;
}

JsonValue JsonValue::number(uint64_t value) {
    JsonValue v;
    v.type_ = Type::Number;
    v.text_ = std::to_string(value);
    return v;
}

JsonValue JsonValue::number(int64_t value) {
    JsonValue v;
    v.type_ = Type::Number;
    v.text_ = std::to_string(value);
    return v;
}

JsonValue JsonValue::number(double value) {
    if (!std::isfinite(value)) return JsonValue();
    // The shortest of 15 or 17 significant digits that reads back exactly
    char buffer[32];
    snprintf_l(buffer, sizeof(buffer), cLocale(), "%.15g", value);
    if (strtod_l(buffer, nullptr, cLocale()) != value) {
        snprintf_l(buffer, sizeof(buffer), cLocale(), "%.17g", value);
    }
    JsonValue v;
    v.type_ = Type::Number;
    v.text_ = buffer;
    return v;
}

JsonValue JsonValue::string(std::string value) {
    JsonValue v;
    v.type_ = Type::String;
    v.text_ = std::move(value);
    return v;
}

JsonValue JsonValue::array(Array items) {
    JsonValue v;
    v.type_ = Type::Array;
    v.array_ = std::move(items);
    return v;
}

JsonValue JsonValue::object(Object members) {
    JsonValue v;
    v.type_ = Type::Object;
    v.object_ = std::move(members);
    return v;
}

std::optional<bool> JsonValue::asBool() const {
    if (type_ != Type::Bool) return std::nullopt;
    return bool_;
}

std::optional<uint64_t> JsonValue::asUInt64() const {
    if (type_ != Type::Number || !isIntegerLiteral(text_)) return std::nullopt;
    uint64_t value = 0;
    const char* end = text_.data() + text_.size();
    const auto result = std::from_chars(text_.data(), end, value);
    if (result.ec != std::errc() || result.ptr != end) return std::nullopt;  // negative or too big
    return value;
}

std::optional<int64_t> JsonValue::asInt64() const {
    if (type_ != Type::Number || !isIntegerLiteral(text_)) return std::nullopt;
    int64_t value = 0;
    const char* end = text_.data() + text_.size();
    const auto result = std::from_chars(text_.data(), end, value);
    if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
    return value;
}

std::optional<double> JsonValue::asDouble() const {
    if (type_ != Type::Number) return std::nullopt;
    const double value = strtod_l(text_.c_str(), nullptr, cLocale());
    if (!std::isfinite(value)) return std::nullopt;  // 1e999
    return value;
}

const std::string* JsonValue::asString() const {
    return type_ == Type::String ? &text_ : nullptr;
}

const JsonValue::Array* JsonValue::asArray() const {
    return type_ == Type::Array ? &array_ : nullptr;
}

const JsonValue::Object* JsonValue::asObject() const {
    return type_ == Type::Object ? &object_ : nullptr;
}

const JsonValue* JsonValue::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (auto it = object_.rbegin(); it != object_.rend(); ++it) {
        if (it->key == key) return &it->value;
    }
    return nullptr;
}

JsonValue& JsonValue::push(JsonValue item) {
    array_.push_back(std::move(item));
    return *this;
}

JsonValue& JsonValue::add(std::string key, JsonValue value) {
    object_.push_back(Member{std::move(key), std::move(value)});
    return *this;
}

// ============================================================================
// Writer
// ============================================================================

std::string JsonValue::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void JsonValue::dumpTo(std::string& out, int indent, int level) const {
    const auto newline = [&](int atLevel) {
        out += '\n';
        out.append(static_cast<size_t>(indent * atLevel), ' ');
    };

    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: out += text_; break;
        case Type::String: appendEscaped(out, text_); break;
        case Type::Array: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            bool flat = true;
            for (const auto& item : array_) {
                if (item.type_ == Type::Array || item.type_ == Type::Object) flat = false;
            }
            out += '[';
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i > 0) out += flat ? ", " : ",";
                if (!flat) newline(level + 1);
                array_[i].dumpTo(out, indent, level + 1);
            }
            if (!flat) newline(level);
            out += ']';
            break;
        }
        case Type::Object: {
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            for (size_t i = 0; i < object_.size(); ++i) {
                if (i > 0) out += ',';
                newline(level + 1);
                appendEscaped(out, object_[i].key);
                out += ": ";
                object_[i].value.dumpTo(out, indent, level + 1);
            }
            newline(level);
            out += '}';
            break;
        }
    }
}

} // namespace AES67
