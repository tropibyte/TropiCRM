// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "json/json.hpp"
#include "meta/object.hpp"

namespace tcrm::meta {

/// The object model of one org: the standard objects plus whatever the
/// org's admins added through the Tooling API. Not thread-safe; the owning
/// Org guards it.
class Schema {
public:
    /// A schema holding only the standard objects.
    static Schema standard();

    const ObjectDef* object(std::string_view name) const;
    ObjectDef* object(std::string_view name);
    const ObjectDef* object_by_prefix(std::string_view key_prefix) const;
    std::vector<const ObjectDef*> objects() const;

    /// Adds a custom object with the standard system fields and a Name
    /// field. Throws ApiError for an invalid or duplicate name.
    const ObjectDef& create_custom_object(const std::string& name, const std::string& label,
                                          const std::string& plural_label, const std::string& description);

    /// Adds a custom field. Throws ApiError for invalid definitions.
    const FieldDef& add_custom_field(const std::string& object_name, FieldDef field);

    /// Removes a custom field; standard fields can't be removed.
    void remove_custom_field(const std::string& object_name, const std::string& field_name);

    /// Changes the editable attributes of a field (label, help text,
    /// required, unique, picklist values, length). Standard fields accept
    /// only label, help text and picklist values. Throws ApiError.
    const FieldDef& update_field(const std::string& object_name, const std::string& field_name,
                                 const json::Value& changes);

    /// Increments on every change; caches key off it.
    uint64_t version() const { return version_; }

    /// Recomputes every object's child relationships from lookup fields.
    void rebuild_relationships();

    /// The custom objects and fields, for persistence.
    json::Value custom_to_json() const;
    void load_custom_json(const json::Value& doc);

    static json::Value field_to_json(const FieldDef& f);
    static FieldDef field_from_json(const json::Value& v);

private:
    std::map<std::string, ObjectDef> objects_;  // keyed by lowercased name
    int next_custom_prefix_ = 0;
    uint64_t version_ = 1;
};

/// Builds the standard objects (Account, Contact, Lead, Opportunity, ...).
std::vector<ObjectDef> standard_objects();

/// The system fields every object carries.
void add_system_fields(ObjectDef* obj, bool with_owner);

}  // namespace tcrm::meta
