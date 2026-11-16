// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "model/value.hpp"

#include <stdexcept>

#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm {
namespace {

int rank(ValueKind k) {
    switch (k) {
        case ValueKind::Null: return 0;
        case ValueKind::Bool: return 1;
        case ValueKind::Int:
        case ValueKind::Double: return 2;
        case ValueKind::Date: return 3;
        case ValueKind::DateTime: return 4;
        case ValueKind::String: return 5;
    }
    return 6;
}

template <typename T>
int cmp(const T& a, const T& b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

}  // namespace

const char* value_kind_name(ValueKind k) {
    switch (k) {
        case ValueKind::Null: return "null";
        case ValueKind::Bool: return "boolean";
        case ValueKind::Int: return "integer";
        case ValueKind::Double: return "double";
        case ValueKind::String: return "string";
        case ValueKind::Date: return "date";
        case ValueKind::DateTime: return "datetime";
    }
    return "?";
}

FieldValue FieldValue::boolean(bool b) {
    FieldValue v;
    v.kind_ = ValueKind::Bool;
    v.v_ = b;
    return v;
}

FieldValue FieldValue::integer(int64_t i) {
    FieldValue v;
    v.kind_ = ValueKind::Int;
    v.v_ = i;
    return v;
}

FieldValue FieldValue::number(double d) {
    FieldValue v;
    v.kind_ = ValueKind::Double;
    v.v_ = d;
    return v;
}

FieldValue FieldValue::string(std::string s) {
    FieldValue v;
    v.kind_ = ValueKind::String;
    v.v_ = std::move(s);
    return v;
}

FieldValue FieldValue::date(int64_t days) {
    FieldValue v;
    v.kind_ = ValueKind::Date;
    v.v_ = days;
    return v;
}

FieldValue FieldValue::datetime(int64_t ms) {
    FieldValue v;
    v.kind_ = ValueKind::DateTime;
    v.v_ = ms;
    return v;
}

bool FieldValue::as_bool() const {
    if (kind_ != ValueKind::Bool) throw std::logic_error("FieldValue is not a boolean");
    return std::get<bool>(v_);
}

int64_t FieldValue::as_int() const {
    if (kind_ == ValueKind::Int) return std::get<int64_t>(v_);
    if (kind_ == ValueKind::Double) return static_cast<int64_t>(std::get<double>(v_));
    throw std::logic_error("FieldValue is not numeric");
}

double FieldValue::as_double() const {
    if (kind_ == ValueKind::Int) return static_cast<double>(std::get<int64_t>(v_));
    if (kind_ == ValueKind::Double) return std::get<double>(v_);
    throw std::logic_error("FieldValue is not numeric");
}

const std::string& FieldValue::as_string() const {
    if (kind_ != ValueKind::String) throw std::logic_error("FieldValue is not a string");
    return std::get<std::string>(v_);
}

int64_t FieldValue::as_date() const {
    if (kind_ != ValueKind::Date) throw std::logic_error("FieldValue is not a date");
    return std::get<int64_t>(v_);
}

int64_t FieldValue::as_datetime() const {
    if (kind_ != ValueKind::DateTime) throw std::logic_error("FieldValue is not a date-time");
    return std::get<int64_t>(v_);
}

std::string FieldValue::to_text() const {
    switch (kind_) {
        case ValueKind::Null: return "";
        case ValueKind::Bool: return std::get<bool>(v_) ? "true" : "false";
        case ValueKind::Int: return std::to_string(std::get<int64_t>(v_));
        case ValueKind::Double: return str::format_double(std::get<double>(v_));
        case ValueKind::String: return std::get<std::string>(v_);
        case ValueKind::Date: return timeutil::format_date(std::get<int64_t>(v_));
        case ValueKind::DateTime: return timeutil::format_datetime(std::get<int64_t>(v_));
    }
    return "";
}

int FieldValue::compare(const FieldValue& o) const {
    const int ra = rank(kind_);
    const int rb = rank(o.kind_);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (kind_) {
        case ValueKind::Null: return 0;
        case ValueKind::Bool: return cmp(std::get<bool>(v_), std::get<bool>(o.v_));
        case ValueKind::Int:
        case ValueKind::Double:
            if (kind_ == ValueKind::Int && o.kind_ == ValueKind::Int) {
                return cmp(std::get<int64_t>(v_), std::get<int64_t>(o.v_));
            }
            return cmp(as_double(), o.as_double());
        case ValueKind::Date:
        case ValueKind::DateTime: return cmp(std::get<int64_t>(v_), std::get<int64_t>(o.v_));
        case ValueKind::String: return std::get<std::string>(v_).compare(std::get<std::string>(o.v_)) < 0
                                           ? -1
                                           : (std::get<std::string>(v_) == std::get<std::string>(o.v_) ? 0 : 1);
    }
    return 0;
}

size_t FieldValue::hash() const {
    switch (kind_) {
        case ValueKind::Null: return 0x9e3779b9;
        case ValueKind::Bool: return std::hash<bool>()(std::get<bool>(v_));
        case ValueKind::Int: return std::hash<double>()(static_cast<double>(std::get<int64_t>(v_)));
        case ValueKind::Double: return std::hash<double>()(std::get<double>(v_));
        case ValueKind::String: return std::hash<std::string>()(std::get<std::string>(v_));
        case ValueKind::Date:
        case ValueKind::DateTime: return std::hash<int64_t>()(std::get<int64_t>(v_)) ^ static_cast<size_t>(kind_);
    }
    return 0;
}

}  // namespace tcrm
