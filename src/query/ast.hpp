// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/value.hpp"

namespace tcrm::query {

/// A field reference, possibly through relationships: Account.Owner.Name.
struct FieldPath {
    std::vector<std::string> parts;

    std::string text() const;
    bool simple() const { return parts.size() == 1; }
    const std::string& last() const { return parts.back(); }
};

/// The right-hand side of a comparison.
struct Operand {
    enum class Kind { Value, DateRange };
    Kind kind = Kind::Value;
    FieldValue value;
    // DateRange: local calendar days [start_day, end_day). Filled in at
    // execution time from `literal` and the org's time zone.
    std::string literal;  // e.g. "TODAY", "LAST_N_DAYS:30"
    bool is_date_literal = false;
    bool is_datetime_literal = false;
    std::string raw;  // the token as written, for error messages
};

struct Query;

struct Condition {
    enum class Kind { And, Or, Not, Compare, In, Like, Includes, Excludes };
    Kind kind = Kind::Compare;
    std::vector<Condition> children;  // And / Or / Not

    // Leaf conditions.
    FieldPath field;
    std::string aggregate;  // HAVING COUNT(Id) > 1: "COUNT"; empty for plain fields
    std::string op;         // =, !=, <, <=, >, >=
    bool negated = false;   // NOT IN, NOT LIKE
    std::vector<Operand> values;
};

struct SelectItem {
    enum class Kind { Field, Aggregate, Subquery };
    Kind kind = Kind::Field;
    FieldPath path;
    std::string function;  // COUNT, COUNT_DISTINCT, SUM, AVG, MIN, MAX
    bool count_star = false;
    std::string alias;
    std::shared_ptr<Query> subquery;

    /// "COUNT(Id)", "SUM(Amount)", "COUNT()" — used to match HAVING and
    /// ORDER BY expressions.
    std::string expression_key() const;
};

struct OrderItem {
    FieldPath path;
    std::string aggregate;  // ORDER BY COUNT(Id) DESC
    bool descending = false;
    enum class Nulls { Default, First, Last } nulls = Nulls::Default;
};

struct Query {
    std::vector<SelectItem> select;
    std::string from;
    std::optional<Condition> where;
    std::vector<FieldPath> group_by;
    std::optional<Condition> having;
    std::vector<OrderItem> order_by;
    int64_t limit = -1;
    int64_t offset = 0;
    bool all_rows = false;

    bool is_aggregate() const;
    bool is_count_only() const;  // SELECT COUNT() FROM ...
};

/// Parses TQL, the SOQL-like query language. Throws ApiError
/// (MALFORMED_QUERY) on syntax errors.
Query parse(const std::string& text);

/// Quotes a value for safe inclusion in a TQL string literal.
std::string quote_literal(const std::string& value);

}  // namespace tcrm::query
