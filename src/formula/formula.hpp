// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "model/value.hpp"
#include "util/time.hpp"

namespace tcrm::formula {

class FormulaError : public std::runtime_error {
public:
    FormulaError(const std::string& msg, size_t pos)
        : std::runtime_error(msg + " (at character " + std::to_string(pos + 1) + ")"), pos_(pos) {}
    size_t position() const { return pos_; }

private:
    size_t pos_;
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
    enum class Kind { Literal, Field, Unary, Binary, Call };
    Kind kind = Kind::Literal;
    size_t pos = 0;
    FieldValue literal;          // Literal
    std::string name;            // Field path, operator or function name
    std::vector<NodePtr> args;   // operands / arguments
};

/// What a formula can see while it runs.
class Context {
public:
    virtual ~Context() = default;

    /// A field of the current record, or a dotted path through lookups
    /// ("Account.Owner.Name"). Unknown fields throw FormulaError.
    virtual FieldValue field(const std::string& path) = 0;

    /// The value before the current change (PRIORVALUE, ISCHANGED).
    virtual FieldValue prior(const std::string& field) { return this->field(field); }
    virtual bool is_new() { return false; }

    /// $User.Id and friends.
    virtual FieldValue global(const std::string& name) {
        (void)name;
        return FieldValue::null();
    }

    virtual int64_t now_ms() { return timeutil::now_ms(); }
    virtual const timeutil::TimeZone& timezone() {
        static const timeutil::TimeZone utc = timeutil::TimeZone::utc();
        return utc;
    }
};

/// A parsed formula. Parsing is relatively expensive; compile once and
/// evaluate many times.
class Formula {
public:
    static Formula compile(const std::string& source);

    FieldValue evaluate(Context& ctx) const;

    /// Field paths the formula reads (for dependency tracking).
    std::vector<std::string> referenced_fields() const;

    const std::string& source() const { return source_; }
    bool valid() const { return root_ != nullptr; }

private:
    std::string source_;
    std::shared_ptr<const Node> root_;
};

/// Interprets a formula result as a boolean: true only for a true Bool.
bool truthy(const FieldValue& v);

/// Function names known to the evaluator, for validation messages.
const std::vector<std::string>& function_names();

}  // namespace tcrm::formula
