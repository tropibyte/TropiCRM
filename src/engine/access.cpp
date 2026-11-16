// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/access.hpp"

#include <algorithm>

#include "model/errors.hpp"
#include "util/id.hpp"

namespace tcrm::engine {

using security::Access;

Principal Principal::system_for(std::shared_ptr<Org> org, const std::string& acting_user_id) {
    Principal p;
    p.org = std::move(org);
    p.user_id = acting_user_id;
    p.username = "system";
    p.system = true;
    p.profile.modify_all_data = true;
    p.profile.view_all_data = true;
    p.profile.default_object = security::ObjectPermissions::full();
    return p;
}

bool AccessChecker::object_allowed(const meta::ObjectDef& obj, ObjectOp op) const {
    if (who_.system) return true;
    if (obj.name == "User") return op == ObjectOp::Read;
    const security::ObjectPermissions p = who_.profile.object_permissions(obj.name);
    switch (op) {
        case ObjectOp::Read: return p.read;
        case ObjectOp::Create: return p.create;
        case ObjectOp::Edit: return p.edit;
        case ObjectOp::Delete: return p.del;
    }
    return false;
}

void AccessChecker::require_object(const meta::ObjectDef& obj, ObjectOp op) const {
    if (object_allowed(obj, op)) return;
    const char* verb = op == ObjectOp::Read     ? "read"
                       : op == ObjectOp::Create ? "create"
                       : op == ObjectOp::Edit   ? "edit"
                                                : "delete";
    throw ApiError(403, "INSUFFICIENT_ACCESS_OR_READONLY",
                   std::string("You don't have permission to ") + verb + " " + obj.label + " records");
}

bool AccessChecker::field_readable(const meta::ObjectDef& obj, const meta::FieldDef& field) const {
    if (who_.system || field.name == "Id") return true;
    return who_.profile.field_permission(obj.name, field.name).read;
}

bool AccessChecker::field_editable(const meta::ObjectDef& obj, const meta::FieldDef& field) const {
    if (who_.system) return true;
    if (!field.is_createable()) return false;
    const security::FieldPermission fp = who_.profile.field_permission(obj.name, field.name);
    return fp.read && fp.edit;
}

Access AccessChecker::record_access(const meta::ObjectDef& obj, const Record& r) const {
    return compute(obj, r, 0);
}

Access AccessChecker::access_via_parent(const meta::ObjectDef& obj, const Record& r, int depth) const {
    if (depth > 4) return Access::None;
    // Prefer a master-detail field; otherwise the first non-system lookup.
    const meta::FieldDef* link = nullptr;
    for (const meta::FieldDef& f : obj.fields) {
        if (f.type == meta::FieldType::MasterDetail) {
            link = &f;
            break;
        }
    }
    if (link == nullptr) {
        for (const meta::FieldDef& f : obj.fields) {
            if (f.type == meta::FieldType::Lookup && !f.system && f.name != "OwnerId") {
                link = &f;
                break;
            }
        }
    }
    if (link == nullptr) return Access::None;
    const FieldValue& v = r.get(link->name);
    if (v.kind() != ValueKind::String) return Access::None;
    const std::string& parent_id = v.as_string();
    const meta::ObjectDef* parent_obj = org_.schema.object_by_prefix(ids::key_prefix(parent_id));
    if (parent_obj == nullptr) return Access::None;
    const storage::Table* t = org_.db.find_table(parent_obj->name);
    const Record* parent = t ? t->get(parent_id) : nullptr;
    if (parent == nullptr) return Access::None;
    return compute(*parent_obj, *parent, depth + 1);
}

Access AccessChecker::compute(const meta::ObjectDef& obj, const Record& r, int depth) const {
    if (who_.system) return Access::All;
    if (obj.name == "User") return Access::Read;

    const security::ObjectPermissions perms = who_.profile.object_permissions(obj.name);
    if (perms.modify_all) return Access::All;

    Access best = perms.view_all ? Access::Read : Access::None;
    auto raise = [&best](Access a) { best = std::max(best, a); };

    const std::string owner = r.owner_id();
    if (!owner.empty() && owner == who_.user_id) return Access::All;

    switch (org_.sharing.default_for(obj.name)) {
        case security::OrgDefault::PublicReadWrite: raise(Access::Edit); break;
        case security::OrgDefault::PublicRead: raise(Access::Read); break;
        case security::OrgDefault::ControlledByParent: {
            // Records without an owner field inherit entirely from the parent;
            // owned records (e.g. a Contact with no Account) fall back to
            // private sharing below.
            const Access parent = access_via_parent(obj, r, depth);
            if (obj.field("OwnerId") == nullptr || r.get("AccountId").kind() == ValueKind::String ||
                parent != Access::None) {
                raise(parent == Access::All ? Access::Edit : parent);
            }
            break;
        }
        case security::OrgDefault::Private: break;
    }
    if (best >= Access::Edit) return best;

    // Role hierarchy: managers get edit access to their reports' records.
    if (!owner.empty() && !who_.role_id.empty()) {
        const auth::User* owner_user = org_.users.find(owner);
        if (owner_user != nullptr && org_.roles.is_above(who_.role_id, owner_user->role_id)) raise(Access::Edit);
    }

    // Criteria-based sharing rules.
    for (const security::SharingRule& rule : org_.sharing.rules) {
        if (rule.object != obj.name || rule.access <= best) continue;
        if (r.text(rule.field) != rule.value) continue;
        const std::vector<std::string> roles = org_.roles.role_and_subordinates(rule.role_id);
        if (std::find(roles.begin(), roles.end(), who_.role_id) != roles.end()) raise(rule.access);
    }

    // Manual shares.
    for (const security::ManualShare& s : org_.sharing.manual) {
        if (s.record_id == r.id && s.user_id == who_.user_id) raise(s.access);
    }
    return best;
}

}  // namespace tcrm::engine
