// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "security/sharing.hpp"

#include <algorithm>

#include "model/errors.hpp"
#include "util/strutil.hpp"

namespace tcrm::security {

const char* access_name(Access a) {
    switch (a) {
        case Access::None: return "None";
        case Access::Read: return "Read";
        case Access::Edit: return "Edit";
        case Access::All: return "All";
    }
    return "None";
}

bool parse_access(const std::string& s, Access* out) {
    if (str::iequals(s, "None")) *out = Access::None;
    else if (str::iequals(s, "Read")) *out = Access::Read;
    else if (str::iequals(s, "Edit")) *out = Access::Edit;
    else if (str::iequals(s, "All")) *out = Access::All;
    else return false;
    return true;
}

const Role* RoleHierarchy::find(const std::string& id) const {
    for (const Role& r : roles_) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

void RoleHierarchy::put(Role role) {
    if (!role.parent_id.empty() && find(role.parent_id) == nullptr) {
        throw errors::invalid_field("ParentRoleId", "No such role: " + role.parent_id);
    }
    if (role.parent_id == role.id) throw errors::invalid_field("ParentRoleId", "A role can't be its own parent");
    for (Role& existing : roles_) {
        if (existing.id == role.id) {
            existing = std::move(role);
            return;
        }
    }
    roles_.push_back(std::move(role));
}

void RoleHierarchy::remove(const std::string& id) {
    for (const Role& r : roles_) {
        if (r.parent_id == id) {
            throw errors::bad_request("DELETE_FAILED", "Role " + id + " has child roles; move or delete them first");
        }
    }
    roles_.erase(std::remove_if(roles_.begin(), roles_.end(), [&](const Role& r) { return r.id == id; }),
                 roles_.end());
}

bool RoleHierarchy::is_above(const std::string& manager_role, const std::string& subordinate_role) const {
    if (manager_role.empty() || subordinate_role.empty()) return false;
    std::string cur = subordinate_role;
    while (!cur.empty()) {
        if (cur == manager_role) return true;
        const Role* r = find(cur);
        if (r == nullptr) return false;
        cur = r->parent_id;
    }
    return false;
}

std::vector<std::string> RoleHierarchy::role_and_subordinates(const std::string& role) const {
    std::vector<std::string> out;
    if (role.empty()) return out;
    out.push_back(role);
    for (size_t i = 0; i < out.size(); ++i) {
        for (const Role& r : roles_) {
            if (r.parent_id == out[i] && std::find(out.begin(), out.end(), r.id) == out.end()) out.push_back(r.id);
        }
    }
    return out;
}

json::Value RoleHierarchy::to_json() const {
    json::Value arr = json::Value::array();
    for (const Role& r : roles_) {
        json::Value v = json::Value::object();
        v.set("id", r.id);
        v.set("name", r.name);
        v.set("parentRoleId", r.parent_id.empty() ? json::Value() : json::Value(r.parent_id));
        arr.push_back(std::move(v));
    }
    return arr;
}

void RoleHierarchy::load_json(const json::Value& v) {
    roles_.clear();
    if (!v.is_array()) return;
    for (const json::Value& jr : v.as_array()) {
        Role r;
        r.id = jr.get_string("id");
        r.name = jr.get_string("name");
        r.parent_id = jr.get_string("parentRoleId");
        roles_.push_back(std::move(r));
    }
}

const char* org_default_name(OrgDefault d) {
    switch (d) {
        case OrgDefault::Private: return "Private";
        case OrgDefault::PublicRead: return "PublicRead";
        case OrgDefault::PublicReadWrite: return "PublicReadWrite";
        case OrgDefault::ControlledByParent: return "ControlledByParent";
    }
    return "Private";
}

bool parse_org_default(const std::string& s, OrgDefault* out) {
    if (str::iequals(s, "Private")) *out = OrgDefault::Private;
    else if (str::iequals(s, "PublicRead") || str::iequals(s, "Read")) *out = OrgDefault::PublicRead;
    else if (str::iequals(s, "PublicReadWrite") || str::iequals(s, "ReadWrite")) *out = OrgDefault::PublicReadWrite;
    else if (str::iequals(s, "ControlledByParent")) *out = OrgDefault::ControlledByParent;
    else return false;
    return true;
}

OrgDefault SharingModel::default_for(const std::string& object) const {
    auto it = defaults.find(object);
    return it == defaults.end() ? OrgDefault::Private : it->second;
}

SharingModel SharingModel::standard() {
    SharingModel m;
    m.defaults["Account"] = OrgDefault::Private;
    m.defaults["Contact"] = OrgDefault::ControlledByParent;
    m.defaults["Lead"] = OrgDefault::PublicReadWrite;
    m.defaults["Opportunity"] = OrgDefault::Private;
    m.defaults["OpportunityLineItem"] = OrgDefault::ControlledByParent;
    m.defaults["Case"] = OrgDefault::Private;
    m.defaults["Campaign"] = OrgDefault::PublicRead;
    m.defaults["Product2"] = OrgDefault::PublicRead;
    m.defaults["Task"] = OrgDefault::Private;
    m.defaults["Event"] = OrgDefault::Private;
    m.defaults["Note"] = OrgDefault::ControlledByParent;
    m.defaults["Attachment"] = OrgDefault::ControlledByParent;
    m.defaults["User"] = OrgDefault::PublicRead;
    return m;
}

json::Value SharingModel::to_json() const {
    json::Value v = json::Value::object();
    json::Value d = json::Value::object();
    for (const auto& [obj, def] : defaults) d.set(obj, org_default_name(def));
    v.set("organizationWideDefaults", std::move(d));
    json::Value rs = json::Value::array();
    for (const SharingRule& r : rules) {
        json::Value jr = json::Value::object();
        jr.set("id", r.id);
        jr.set("name", r.name);
        jr.set("object", r.object);
        jr.set("field", r.field);
        jr.set("value", r.value);
        jr.set("roleId", r.role_id);
        jr.set("access", access_name(r.access));
        rs.push_back(std::move(jr));
    }
    v.set("sharingRules", std::move(rs));
    json::Value ms = json::Value::array();
    for (const ManualShare& s : manual) {
        json::Value js = json::Value::object();
        js.set("recordId", s.record_id);
        js.set("userId", s.user_id);
        js.set("access", access_name(s.access));
        js.set("reason", s.reason);
        ms.push_back(std::move(js));
    }
    v.set("manualShares", std::move(ms));
    return v;
}

SharingModel SharingModel::from_json(const json::Value& v) {
    SharingModel m;
    if (const json::Value* d = v.find("organizationWideDefaults"); d && d->is_object()) {
        for (const auto& [obj, def] : d->as_object()) {
            OrgDefault od;
            if (def.is_string() && parse_org_default(def.as_string(), &od)) m.defaults[obj] = od;
        }
    }
    if (const json::Value* rs = v.find("sharingRules"); rs && rs->is_array()) {
        for (const json::Value& jr : rs->as_array()) {
            SharingRule r;
            r.id = jr.get_string("id");
            r.name = jr.get_string("name");
            r.object = jr.get_string("object");
            r.field = jr.get_string("field");
            r.value = jr.get_string("value");
            r.role_id = jr.get_string("roleId");
            parse_access(jr.get_string("access", "Read"), &r.access);
            m.rules.push_back(std::move(r));
        }
    }
    if (const json::Value* ms = v.find("manualShares"); ms && ms->is_array()) {
        for (const json::Value& js : ms->as_array()) {
            ManualShare s;
            s.record_id = js.get_string("recordId");
            s.user_id = js.get_string("userId");
            parse_access(js.get_string("access", "Read"), &s.access);
            s.reason = js.get_string("reason");
            m.manual.push_back(std::move(s));
        }
    }
    return m;
}

}  // namespace tcrm::security
