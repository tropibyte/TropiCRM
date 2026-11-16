// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/records.hpp"

#include "meta/codec.hpp"
#include "util/id.hpp"

namespace tcrm::engine {

json::Value record_attributes(const std::string& object, const std::string& id) {
    json::Value a = json::Value::object();
    a.set("type", object);
    a.set("url", std::string(kApiBase) + "/sobjects/" + object + "/" + id);
    return a;
}

Record user_record(const auth::User& u, const Org& org) {
    Record r;
    r.id = u.id;
    r.object = "User";
    r.set("Id", FieldValue::string(u.id));
    r.set("Username", FieldValue::string(u.username));
    r.set("Name", FieldValue::string(u.full_name()));
    if (!u.first_name.empty()) r.set("FirstName", FieldValue::string(u.first_name));
    r.set("LastName", FieldValue::string(u.last_name));
    if (!u.email.empty()) r.set("Email", FieldValue::string(u.email));
    if (!u.title.empty()) r.set("Title", FieldValue::string(u.title));
    if (!u.department.empty()) r.set("Department", FieldValue::string(u.department));
    r.set("IsActive", FieldValue::boolean(u.active));
    if (const security::Profile* p = org.profiles.find(u.profile_id)) r.set("ProfileName", FieldValue::string(p->name));
    if (!u.role_id.empty()) r.set("UserRoleId", FieldValue::string(u.role_id));
    if (!u.manager_id.empty()) r.set("ManagerId", FieldValue::string(u.manager_id));
    r.set("TimeZoneSidKey", FieldValue::string(u.timezone));
    return r;
}

const meta::ObjectDef* object_for_id(const Org& org, const std::string& id) {
    return org.schema.object_by_prefix(ids::key_prefix(id));
}

std::optional<Record> load_record(const Org& org, const std::string& id, const meta::ObjectDef** object_out) {
    std::string id18;
    if (!ids::normalize(id, &id18)) return std::nullopt;
    const meta::ObjectDef* obj = object_for_id(org, id18);
    if (obj == nullptr) return std::nullopt;
    if (object_out) *object_out = obj;
    if (obj->name == "User") {
        const auth::User* u = org.users.find(id18);
        if (u == nullptr) return std::nullopt;
        return user_record(*u, org);
    }
    const storage::Table* t = org.db.find_table(obj->name);
    if (t == nullptr) return std::nullopt;
    const Record* r = t->get(id18);
    if (r == nullptr) return std::nullopt;
    return *r;
}

json::Value record_to_api_json(const meta::ObjectDef& obj, const Record& r, const AccessChecker& access,
                               const std::vector<std::string>* fields) {
    json::Value out = json::Value::object();
    out.set("attributes", record_attributes(obj.name, r.id));
    auto emit = [&](const meta::FieldDef& f) {
        if (!access.field_readable(obj, f)) return;
        out.set(f.name, meta::value_to_json(f, r.get(f.name)));
    };
    if (fields == nullptr) {
        for (const meta::FieldDef& f : obj.fields) emit(f);
    } else {
        for (const std::string& name : *fields) {
            if (const meta::FieldDef* f = obj.field(name)) emit(*f);
        }
    }
    return out;
}

std::string record_name(const meta::ObjectDef& obj, const Record& r) {
    const std::string text = r.text(obj.name_field);
    return text.empty() ? r.id : text;
}

}  // namespace tcrm::engine
