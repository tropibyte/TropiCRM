// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>

#include "formula/formula.hpp"

namespace tcrm::formula {

struct FunctionInfo {
    const char* name;
    int min_args;
    int max_args;  // -1 = variadic
};

const FunctionInfo* find_function(const std::string& upper_name);

FieldValue eval_node(const Node& n, Context& ctx);

// Coercions shared by operators and functions.
double to_number(const FieldValue& v, size_t pos);
std::string to_text(const FieldValue& v);
bool is_blank(const FieldValue& v);

}  // namespace tcrm::formula
