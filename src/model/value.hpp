// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <variant>

namespace tcrm {

enum class ValueKind { Null, Bool, Int, Double, String, Date, DateTime };

const char* value_kind_name(ValueKind k);

/// A single field value as stored in a record. Dates are days since the
/// epoch; date-times are UTC milliseconds since the epoch.
class FieldValue {
public:
    FieldValue() = default;

    static FieldValue null() { return FieldValue(); }
    static FieldValue boolean(bool b);
    static FieldValue integer(int64_t v);
    static FieldValue number(double v);
    static FieldValue string(std::string s);
    static FieldValue date(int64_t days);
    static FieldValue datetime(int64_t ms);

    ValueKind kind() const { return kind_; }
    bool is_null() const { return kind_ == ValueKind::Null; }
    bool is_numeric() const { return kind_ == ValueKind::Int || kind_ == ValueKind::Double; }
    bool is_temporal() const { return kind_ == ValueKind::Date || kind_ == ValueKind::DateTime; }

    bool as_bool() const;
    int64_t as_int() const;
    double as_double() const;  // Int or Double
    const std::string& as_string() const;
    int64_t as_date() const;
    int64_t as_datetime() const;

    /// Display text: "true", "42", "3.5", "2026-01-31", ISO date-time.
    std::string to_text() const;

    /// Total order used for sorting and indexes: nulls first, numbers
    /// compared numerically across Int/Double, strings by byte value.
    int compare(const FieldValue& other) const;

    bool operator==(const FieldValue& o) const { return compare(o) == 0; }
    bool operator!=(const FieldValue& o) const { return compare(o) != 0; }
    bool operator<(const FieldValue& o) const { return compare(o) < 0; }

    size_t hash() const;

private:
    ValueKind kind_ = ValueKind::Null;
    std::variant<std::monostate, bool, int64_t, double, std::string> v_;
};

struct FieldValueHash {
    size_t operator()(const FieldValue& v) const { return v.hash(); }
};

}  // namespace tcrm
