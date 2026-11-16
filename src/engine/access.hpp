// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <memory>
#include <string>

#include "engine/org.hpp"
#include "meta/object.hpp"
#include "model/record.hpp"
#include "security/profile.hpp"
#include "security/sharing.hpp"

namespace tcrm::engine {

/// Who is making a request.
struct Principal {
    std::shared_ptr<Org> org;
    std::string user_id;
    std::string username;
    std::string role_id;
    std::string timezone;
    security::Profile profile;
    std::string token_id;
    bool via_api_key = false;
    bool system = false;  // internal operations that bypass security

    /// A principal for platform jobs that act on an org without a user.
    static Principal system_for(std::shared_ptr<Org> org, const std::string& acting_user_id = {});
};

enum class ObjectOp { Read, Create, Edit, Delete };

/// Answers "may this principal do X?" for objects, fields and records.
/// The caller must hold the org's lock (shared is enough).
class AccessChecker {
public:
    AccessChecker(const Org& org, const Principal& who) : org_(org), who_(who) {}

    bool object_allowed(const meta::ObjectDef& obj, ObjectOp op) const;

    /// Throws ApiError(403) when the operation isn't allowed.
    void require_object(const meta::ObjectDef& obj, ObjectOp op) const;

    bool field_readable(const meta::ObjectDef& obj, const meta::FieldDef& field) const;
    bool field_editable(const meta::ObjectDef& obj, const meta::FieldDef& field) const;

    security::Access record_access(const meta::ObjectDef& obj, const Record& r) const;

    bool can_read(const meta::ObjectDef& obj, const Record& r) const {
        return record_access(obj, r) >= security::Access::Read;
    }
    bool can_edit(const meta::ObjectDef& obj, const Record& r) const {
        return record_access(obj, r) >= security::Access::Edit;
    }
    bool can_delete(const meta::ObjectDef& obj, const Record& r) const {
        return record_access(obj, r) == security::Access::All;
    }

private:
    security::Access access_via_parent(const meta::ObjectDef& obj, const Record& r, int depth) const;
    security::Access compute(const meta::ObjectDef& obj, const Record& r, int depth) const;

    const Org& org_;
    const Principal& who_;
};

}  // namespace tcrm::engine
