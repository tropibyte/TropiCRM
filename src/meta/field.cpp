// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "meta/field.hpp"

#include <array>

#include "meta/object.hpp"
#include "util/strutil.hpp"

namespace tcrm::meta {
namespace {

struct TypeName {
    FieldType type;
    const char* name;
};

constexpr std::array<TypeName, 20> kTypeNames = {{
    {FieldType::Id, "id"},
    {FieldType::Text, "string"},
    {FieldType::TextArea, "textarea"},
    {FieldType::LongTextArea, "longtextarea"},
    {FieldType::Email, "email"},
    {FieldType::Phone, "phone"},
    {FieldType::Url, "url"},
    {FieldType::Number, "double"},
    {FieldType::Currency, "currency"},
    {FieldType::Percent, "percent"},
    {FieldType::Checkbox, "boolean"},
    {FieldType::Date, "date"},
    {FieldType::DateTime, "datetime"},
    {FieldType::Picklist, "picklist"},
    {FieldType::MultiPicklist, "multipicklist"},
    {FieldType::Lookup, "reference"},
    {FieldType::MasterDetail, "masterdetail"},
    {FieldType::Formula, "formula"},
    {FieldType::AutoNumber, "autonumber"},
    {FieldType::RollupSummary, "summary"},
}};

}  // namespace

const char* field_type_name(FieldType t) {
    for (const TypeName& tn : kTypeNames) {
        if (tn.type == t) return tn.name;
    }
    return "string";
}

bool parse_field_type(std::string_view name, FieldType* out) {
    for (const TypeName& tn : kTypeNames) {
        if (str::iequals(name, tn.name)) {
            *out = tn.type;
            return true;
        }
    }
    // A few friendlier aliases accepted by the Tooling API.
    if (str::iequals(name, "text")) *out = FieldType::Text;
    else if (str::iequals(name, "number")) *out = FieldType::Number;
    else if (str::iequals(name, "checkbox")) *out = FieldType::Checkbox;
    else if (str::iequals(name, "lookup")) *out = FieldType::Lookup;
    else if (str::iequals(name, "rollupsummary")) *out = FieldType::RollupSummary;
    else return false;
    return true;
}

bool is_text_type(FieldType t) {
    switch (t) {
        case FieldType::Id:
        case FieldType::Text:
        case FieldType::TextArea:
        case FieldType::LongTextArea:
        case FieldType::Email:
        case FieldType::Phone:
        case FieldType::Url:
        case FieldType::Picklist:
        case FieldType::MultiPicklist:
        case FieldType::Lookup:
        case FieldType::MasterDetail:
        case FieldType::AutoNumber:
            return true;
        default:
            return false;
    }
}

bool is_numeric_type(FieldType t) {
    return t == FieldType::Number || t == FieldType::Currency || t == FieldType::Percent;
}

bool is_reference_type(FieldType t) {
    return t == FieldType::Lookup || t == FieldType::MasterDetail;
}

FieldType FieldDef::value_type() const {
    if (type == FieldType::Formula) return formula_return;
    if (type == FieldType::AutoNumber) return FieldType::Text;
    if (type == FieldType::RollupSummary) return FieldType::Number;
    return type;
}

const PicklistValue* FieldDef::find_picklist_value(std::string_view v) const {
    for (const PicklistValue& p : picklist) {
        if (p.value == v) return &p;
    }
    return nullptr;
}

const FieldDef* ObjectDef::field(std::string_view field_name) const {
    auto it = index_.find(str::to_lower(field_name));
    return it == index_.end() ? nullptr : &fields[it->second];
}

FieldDef* ObjectDef::field(std::string_view field_name) {
    auto it = index_.find(str::to_lower(field_name));
    return it == index_.end() ? nullptr : &fields[it->second];
}

const FieldDef* ObjectDef::relationship_field(std::string_view relationship_name) const {
    for (const FieldDef& f : fields) {
        if (is_reference_type(f.type) && str::iequals(f.relationship_name, relationship_name)) return &f;
    }
    return nullptr;
}

void ObjectDef::add_field(FieldDef f) {
    index_[str::to_lower(f.name)] = fields.size();
    fields.push_back(std::move(f));
}

void ObjectDef::rebuild_index() {
    index_.clear();
    for (size_t i = 0; i < fields.size(); ++i) index_[str::to_lower(fields[i].name)] = i;
}

}  // namespace tcrm::meta
