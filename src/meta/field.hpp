// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tcrm::meta {

enum class FieldType {
    Id,
    Text,
    TextArea,
    LongTextArea,
    Email,
    Phone,
    Url,
    Number,
    Currency,
    Percent,
    Checkbox,
    Date,
    DateTime,
    Picklist,
    MultiPicklist,
    Lookup,
    MasterDetail,
    Formula,
    AutoNumber,
    RollupSummary,
};

const char* field_type_name(FieldType t);
bool parse_field_type(std::string_view name, FieldType* out);

/// True for types whose values are strings in a record.
bool is_text_type(FieldType t);
bool is_numeric_type(FieldType t);
bool is_reference_type(FieldType t);

struct PicklistValue {
    std::string value;
    std::string label;
    bool is_default = false;
    bool active = true;
};

enum class DeleteRule { SetNull, Restrict, Cascade };

struct RollupSpec {
    std::string child_object;       // e.g. "Opportunity"
    std::string relationship_field;  // lookup on the child, e.g. "AccountId"
    std::string op;                  // COUNT, SUM, MIN, MAX
    std::string field;               // child field to aggregate (not for COUNT)
};

struct FieldDef {
    std::string name;
    std::string label;
    FieldType type = FieldType::Text;
    int length = 255;    // text types
    int precision = 18;  // numeric types: total digits
    int scale = 2;       // numeric types: digits after the point

    bool required = false;
    bool unique = false;
    bool external_id = false;
    bool indexed = false;
    bool custom = false;
    bool system = false;  // maintained by the platform; never writable
    bool history_tracked = false;
    bool name_field = false;
    bool searchable = false;
    bool read_only = false;

    std::string default_value;  // a formula expression, evaluated on create
    std::string description;
    std::string help_text;

    std::vector<PicklistValue> picklist;
    bool restricted_picklist = false;

    std::string reference_to;       // Lookup / MasterDetail target object
    std::string relationship_name;  // parent-side name for child queries
    DeleteRule delete_rule = DeleteRule::SetNull;

    std::string formula;  // Formula fields
    FieldType formula_return = FieldType::Text;

    std::string autonumber_format;  // "CASE-{00000}"

    RollupSpec rollup;

    /// The effective value type: a formula's return type, Text for
    /// AutoNumber, and the field's own type otherwise.
    FieldType value_type() const;

    bool is_calculated() const {
        return type == FieldType::Formula || type == FieldType::RollupSummary || type == FieldType::AutoNumber;
    }
    bool is_createable() const { return !system && !is_calculated() && !read_only; }
    bool is_updateable() const { return is_createable() && type != FieldType::MasterDetail; }

    const PicklistValue* find_picklist_value(std::string_view v) const;
};

}  // namespace tcrm::meta
