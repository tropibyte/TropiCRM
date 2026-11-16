// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "meta/field.hpp"

namespace tcrm::meta {

struct ChildRelationship {
    std::string child_object;
    std::string field;              // the lookup field on the child
    std::string relationship_name;  // e.g. "Contacts"
    DeleteRule delete_rule = DeleteRule::SetNull;
};

struct ObjectDef {
    std::string name;
    std::string label;
    std::string plural_label;
    std::string key_prefix;
    std::string description;
    bool custom = false;
    bool searchable = true;
    bool queryable = true;
    bool track_history = false;
    std::string name_field = "Name";

    std::vector<FieldDef> fields;
    std::vector<ChildRelationship> children;  // filled in by Schema

    /// Case-insensitive lookup by API name.
    const FieldDef* field(std::string_view name) const;
    FieldDef* field(std::string_view name);

    /// Finds the lookup field whose relationship name matches
    /// (e.g. "Account" -> AccountId on Contact).
    const FieldDef* relationship_field(std::string_view relationship_name) const;

    void add_field(FieldDef f);
    void rebuild_index();

private:
    std::unordered_map<std::string, size_t> index_;  // lowercased name -> position
};

}  // namespace tcrm::meta
