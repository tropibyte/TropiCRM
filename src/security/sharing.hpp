// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "json/json.hpp"

namespace tcrm::security {

enum class Access { None = 0, Read = 1, Edit = 2, All = 3 };

const char* access_name(Access a);
bool parse_access(const std::string& s, Access* out);

struct Role {
    std::string id;
    std::string name;
    std::string parent_id;  // empty for a top-level role
};

/// The org chart used for record visibility: users in a role can see and
/// edit records owned by users in roles below theirs.
class RoleHierarchy {
public:
    const Role* find(const std::string& id) const;
    const std::vector<Role>& all() const { return roles_; }

    /// Adds or replaces a role. Throws ApiError if the parent doesn't exist.
    void put(Role role);
    /// Throws ApiError if the role has children.
    void remove(const std::string& id);

    /// True when `manager_role` sits above `subordinate_role`.
    bool is_above(const std::string& manager_role, const std::string& subordinate_role) const;

    /// `role` and every role below it.
    std::vector<std::string> role_and_subordinates(const std::string& role) const;

    json::Value to_json() const;
    void load_json(const json::Value& v);

private:
    std::vector<Role> roles_;
};

enum class OrgDefault { Private, PublicRead, PublicReadWrite, ControlledByParent };

const char* org_default_name(OrgDefault d);
bool parse_org_default(const std::string& s, OrgDefault* out);

/// "Share <object> records where <field> = <value> with <role> and its
/// subordinates, giving them <access>."
struct SharingRule {
    std::string id;
    std::string name;
    std::string object;
    std::string field;
    std::string value;
    std::string role_id;
    Access access = Access::Read;
};

struct ManualShare {
    std::string record_id;
    std::string user_id;
    Access access = Access::Read;
    std::string reason;
};

/// The org's sharing settings.
struct SharingModel {
    std::map<std::string, OrgDefault> defaults;  // by object; absent = Private
    std::vector<SharingRule> rules;
    std::vector<ManualShare> manual;

    OrgDefault default_for(const std::string& object) const;

    static SharingModel standard();
    json::Value to_json() const;
    static SharingModel from_json(const json::Value& v);
};

}  // namespace tcrm::security
