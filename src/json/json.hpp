// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tcrm::json {

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& msg, size_t offset)
        : std::runtime_error(msg + " at offset " + std::to_string(offset)), offset_(offset) {}
    size_t offset() const { return offset_; }

private:
    size_t offset_;
};

class TypeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A JSON document node. Objects keep their members in insertion order.
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int v) : type_(Type::Number), num_(v), int_(v), is_int_(true) {}
    Value(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)), int_(v), is_int_(true) {}
    Value(uint32_t v) : Value(static_cast<int64_t>(v)) {}
    Value(size_t v) : Value(static_cast<int64_t>(v)) {}
    Value(double v) : type_(Type::Number), num_(v) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), str_(s) {}

    static Value array() {
        Value v;
        v.type_ = Type::Array;
        return v;
    }
    static Value object() {
        Value v;
        v.type_ = Type::Object;
        return v;
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_integer() const { return type_ == Type::Number && is_int_; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const;
    double as_double() const;
    int64_t as_int() const;
    const std::string& as_string() const;
    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();

    // Object access.
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    /// Missing keys yield a shared null value.
    const Value& at(std::string_view key) const;
    /// Inserts a null member when missing. Turns a null value into an object.
    Value& operator[](std::string_view key);
    void set(std::string key, Value v);
    bool erase(std::string_view key);

    std::string get_string(std::string_view key, const std::string& fallback = {}) const;
    int64_t get_int(std::string_view key, int64_t fallback = 0) const;
    double get_double(std::string_view key, double fallback = 0) const;
    bool get_bool(std::string_view key, bool fallback = false) const;

    // Array access.
    void push_back(Value v);
    const Value& operator[](size_t i) const;
    const Value& operator[](int i) const { return (*this)[static_cast<size_t>(i < 0 ? 0 : i)]; }
    const Value& operator[](int i) { return static_cast<const Value&>(*this)[i]; }
    size_t size() const;
    bool empty() const { return size() == 0; }

    std::string dump(bool pretty = false) const;

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

    static const char* type_name(Type t);

private:
    void dump_to(std::string& out, bool pretty, int indent) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    int64_t int_ = 0;
    bool is_int_ = false;
    std::string str_;
    Array arr_;
    Object obj_;
};

/// Parses a complete JSON text. Throws ParseError.
Value parse(std::string_view text);

/// Parses without throwing.
bool try_parse(std::string_view text, Value* out, std::string* error = nullptr);

/// Appends `s` as a quoted JSON string.
void write_string(std::string& out, std::string_view s);

}  // namespace tcrm::json
