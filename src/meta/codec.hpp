// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <string_view>

#include "json/json.hpp"
#include "meta/field.hpp"
#include "model/value.hpp"

namespace tcrm::meta {

/// Converts a client-supplied JSON value into the stored representation for
/// `field`, validating type, length, precision and picklist membership.
/// JSON null maps to a null value. Throws ApiError.
FieldValue value_from_json(const FieldDef& field, const json::Value& v);

/// Same, from a CSV cell. An empty cell is null.
FieldValue value_from_text(const FieldDef& field, std::string_view text);

/// The API representation of a stored value.
json::Value value_to_json(const FieldDef& field, const FieldValue& v);

bool is_valid_email(std::string_view s);

/// Rounds to `scale` decimal places and checks that the integer part fits
/// in precision - scale digits. Throws ApiError on overflow.
double check_precision(const FieldDef& field, double v);

}  // namespace tcrm::meta
