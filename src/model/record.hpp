// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "model/value.hpp"

namespace tcrm {

// System field names present on every record.
namespace sysfield {
inline constexpr const char* kId = "Id";
inline constexpr const char* kOwnerId = "OwnerId";
inline constexpr const char* kCreatedDate = "CreatedDate";
inline constexpr const char* kCreatedById = "CreatedById";
inline constexpr const char* kLastModifiedDate = "LastModifiedDate";
inline constexpr const char* kLastModifiedById = "LastModifiedById";
inline constexpr const char* kSystemModstamp = "SystemModstamp";
inline constexpr const char* kIsDeleted = "IsDeleted";
}  // namespace sysfield

/// One row of an object. Field names are the canonical API names from the
/// schema; callers normalize user-supplied names before touching a record.
struct Record {
    std::string id;
    std::string object;
    std::map<std::string, FieldValue> fields;
    uint64_t version = 0;  // bumped on every write; used for optimistic checks

    const FieldValue& get(std::string_view field) const {
        static const FieldValue kNull;
        auto it = fields.find(std::string(field));
        return it == fields.end() ? kNull : it->second;
    }

    void set(const std::string& field, FieldValue v) { fields[field] = std::move(v); }
    bool has(std::string_view field) const { return fields.count(std::string(field)) != 0; }

    std::string owner_id() const {
        const FieldValue& v = get(sysfield::kOwnerId);
        return v.kind() == ValueKind::String ? v.as_string() : std::string();
    }

    bool is_deleted() const {
        const FieldValue& v = get(sysfield::kIsDeleted);
        return v.kind() == ValueKind::Bool && v.as_bool();
    }

    std::string text(std::string_view field) const { return get(field).to_text(); }
};

}  // namespace tcrm
