// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "json/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>

#include "util/strutil.hpp"

namespace tcrm::json {
namespace {

const Value& null_value() {
    static const Value v;
    return v;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Value parse_document() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != s_.size()) fail("unexpected trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) const { throw ParseError(msg, pos_); }

    void skip_ws() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect_literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) != lit) fail("invalid literal");
        pos_ += lit.size();
    }

    Value parse_value() {
        if (pos_ >= s_.size()) fail("unexpected end of input");
        switch (s_[pos_]) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return Value(parse_string());
            case 't': expect_literal("true"); return Value(true);
            case 'f': expect_literal("false"); return Value(false);
            case 'n': expect_literal("null"); return Value();
            default: return parse_number();
        }
    }

    Value parse_object() {
        ++pos_;  // '{'
        Value obj = Value::object();
        skip_ws();
        if (consume('}')) return obj;
        while (true) {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected member name");
            std::string key = parse_string();
            skip_ws();
            if (!consume(':')) fail("expected ':'");
            skip_ws();
            Value v = parse_value();
            obj.set(std::move(key), std::move(v));
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return obj;
            fail("expected ',' or '}'");
        }
    }

    Value parse_array() {
        ++pos_;  // '['
        Value arr = Value::array();
        skip_ws();
        if (consume(']')) return arr;
        while (true) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) return arr;
            fail("expected ',' or ']'");
        }
    }

    uint32_t parse_hex4() {
        if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("bad hex digit in \\u escape");
        }
        return v;
    }

    std::string parse_string() {
        ++pos_;  // opening quote
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            const uint32_t lo = parse_hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid surrogate pair");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            fail("unpaired surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail("invalid escape");
            }
        }
    }

    Value parse_number() {
        const size_t start = pos_;
        bool is_int = true;
        consume('-');
        if (pos_ >= s_.size()) fail("invalid number");
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            fail("unexpected character");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            is_int = false;
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') fail("invalid number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            is_int = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') fail("invalid number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        const std::string_view text = s_.substr(start, pos_ - start);
        if (is_int) {
            int64_t v = 0;
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
            if (ec == std::errc() && ptr == text.data() + text.size()) return Value(v);
        }
        double d = 0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), d);
        if (ec != std::errc() || !std::isfinite(d)) fail("number out of range");
        return Value(d);
    }

    std::string_view s_;
    size_t pos_ = 0;
};

void indent_to(std::string& out, int indent) {
    out += '\n';
    out.append(static_cast<size_t>(indent) * 2, ' ');
}

}  // namespace

bool Value::as_bool() const {
    if (type_ != Type::Bool) throw TypeError(std::string("expected boolean, got ") + type_name(type_));
    return bool_;
}

double Value::as_double() const {
    if (type_ != Type::Number) throw TypeError(std::string("expected number, got ") + type_name(type_));
    return is_int_ ? static_cast<double>(int_) : num_;
}

int64_t Value::as_int() const {
    if (type_ != Type::Number) throw TypeError(std::string("expected number, got ") + type_name(type_));
    if (is_int_) return int_;
    if (num_ != std::floor(num_) || std::fabs(num_) > 9.2e18) throw TypeError("expected an integer");
    return static_cast<int64_t>(num_);
}

const std::string& Value::as_string() const {
    if (type_ != Type::String) throw TypeError(std::string("expected string, got ") + type_name(type_));
    return str_;
}

const Value::Array& Value::as_array() const {
    if (type_ != Type::Array) throw TypeError(std::string("expected array, got ") + type_name(type_));
    return arr_;
}

Value::Array& Value::as_array() {
    if (type_ != Type::Array) throw TypeError(std::string("expected array, got ") + type_name(type_));
    return arr_;
}

const Value::Object& Value::as_object() const {
    if (type_ != Type::Object) throw TypeError(std::string("expected object, got ") + type_name(type_));
    return obj_;
}

Value::Object& Value::as_object() {
    if (type_ != Type::Object) throw TypeError(std::string("expected object, got ") + type_name(type_));
    return obj_;
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const Member& m : obj_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    if (type_ != Type::Object) return nullptr;
    for (Member& m : obj_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

const Value& Value::at(std::string_view key) const {
    const Value* v = find(key);
    return v ? *v : null_value();
}

Value& Value::operator[](std::string_view key) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) throw TypeError("operator[] on a non-object");
    if (Value* v = find(key)) return *v;
    obj_.emplace_back(std::string(key), Value());
    return obj_.back().second;
}

void Value::set(std::string key, Value v) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) throw TypeError("set() on a non-object");
    if (Value* existing = find(key)) {
        *existing = std::move(v);
        return;
    }
    obj_.emplace_back(std::move(key), std::move(v));
}

bool Value::erase(std::string_view key) {
    if (type_ != Type::Object) return false;
    for (auto it = obj_.begin(); it != obj_.end(); ++it) {
        if (it->first == key) {
            obj_.erase(it);
            return true;
        }
    }
    return false;
}

std::string Value::get_string(std::string_view key, const std::string& fallback) const {
    const Value* v = find(key);
    return (v && v->is_string()) ? v->str_ : fallback;
}

int64_t Value::get_int(std::string_view key, int64_t fallback) const {
    const Value* v = find(key);
    if (!v || !v->is_number()) return fallback;
    return v->is_int_ ? v->int_ : static_cast<int64_t>(v->num_);
}

double Value::get_double(std::string_view key, double fallback) const {
    const Value* v = find(key);
    return (v && v->is_number()) ? v->as_double() : fallback;
}

bool Value::get_bool(std::string_view key, bool fallback) const {
    const Value* v = find(key);
    return (v && v->is_bool()) ? v->bool_ : fallback;
}

void Value::push_back(Value v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) throw TypeError("push_back() on a non-array");
    arr_.push_back(std::move(v));
}

const Value& Value::operator[](size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) return null_value();
    return arr_[i];
}

size_t Value::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

bool Value::operator==(const Value& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == o.bool_;
        case Type::Number: return as_double() == o.as_double();
        case Type::String: return str_ == o.str_;
        case Type::Array: return arr_ == o.arr_;
        case Type::Object:
            if (obj_.size() != o.obj_.size()) return false;
            for (const Member& m : obj_) {
                const Value* other = o.find(m.first);
                if (!other || *other != m.second) return false;
            }
            return true;
    }
    return false;
}

const char* Value::type_name(Type t) {
    switch (t) {
        case Type::Null: return "null";
        case Type::Bool: return "boolean";
        case Type::Number: return "number";
        case Type::String: return "string";
        case Type::Array: return "array";
        case Type::Object: return "object";
    }
    return "?";
}

void write_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    out += '"';
}

void Value::dump_to(std::string& out, bool pretty, int indent) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number:
            if (is_int_) {
                out += std::to_string(int_);
            } else {
                out += str::format_double(num_, 15);
            }
            break;
        case Type::String: write_string(out, str_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i > 0) out += ',';
                if (pretty) indent_to(out, indent + 1);
                arr_[i].dump_to(out, pretty, indent + 1);
            }
            if (pretty && !arr_.empty()) indent_to(out, indent);
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i > 0) out += ',';
                if (pretty) indent_to(out, indent + 1);
                write_string(out, obj_[i].first);
                out += pretty ? ": " : ":";
                obj_[i].second.dump_to(out, pretty, indent + 1);
            }
            if (pretty && !obj_.empty()) indent_to(out, indent);
            out += '}';
            break;
    }
}

std::string Value::dump(bool pretty) const {
    std::string out;
    dump_to(out, pretty, 0);
    return out;
}

Value parse(std::string_view text) {
    Parser p(text);
    return p.parse_document();
}

bool try_parse(std::string_view text, Value* out, std::string* error) {
    try {
        *out = parse(text);
        return true;
    } catch (const ParseError& e) {
        if (error) *error = e.what();
        return false;
    }
}

}  // namespace tcrm::json
