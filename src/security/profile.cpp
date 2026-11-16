// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "security/profile.hpp"

#include "util/strutil.hpp"

namespace tcrm::security {
namespace {

json::Value perms_to_json(const ObjectPermissions& p) {
    json::Value v = json::Value::object();
    v.set("read", p.read);
    v.set("create", p.create);
    v.set("edit", p.edit);
    v.set("delete", p.del);
    v.set("viewAll", p.view_all);
    v.set("modifyAll", p.modify_all);
    return v;
}

ObjectPermissions perms_from_json(const json::Value& v) {
    ObjectPermissions p;
    p.read = v.get_bool("read");
    p.create = v.get_bool("create");
    p.edit = v.get_bool("edit");
    p.del = v.get_bool("delete");
    p.view_all = v.get_bool("viewAll");
    p.modify_all = v.get_bool("modifyAll");
    // Granting a stronger permission implies the weaker ones.
    if (p.modify_all) p.view_all = p.edit = p.del = true;
    if (p.view_all || p.edit || p.create || p.del) p.read = true;
    return p;
}

const char* const kSalesObjects[] = {"Account",  "Contact", "Lead",  "Opportunity", "OpportunityLineItem",
                                     "Product2", "Case",    "Task",  "Event",       "Campaign",
                                     "Note",     "Attachment"};

}  // namespace

ObjectPermissions Profile::object_permissions(const std::string& object) const {
    auto it = objects.find(object);
    ObjectPermissions p = it == objects.end() ? default_object : it->second;
    if (modify_all_data) return ObjectPermissions::full();
    if (view_all_data) {
        p.read = true;
        p.view_all = true;
    }
    return p;
}

FieldPermission Profile::field_permission(const std::string& object, const std::string& field) const {
    auto it = fields.find(object + "." + field);
    return it == fields.end() ? FieldPermission{} : it->second;
}

json::Value Profile::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("description", description);
    json::Value objs = json::Value::object();
    for (const auto& [obj, p] : objects) objs.set(obj, perms_to_json(p));
    v.set("objectPermissions", std::move(objs));
    v.set("defaultObjectPermissions", perms_to_json(default_object));
    json::Value flds = json::Value::object();
    for (const auto& [name_, fp] : fields) {
        json::Value f = json::Value::object();
        f.set("read", fp.read);
        f.set("edit", fp.edit);
        flds.set(name_, std::move(f));
    }
    v.set("fieldPermissions", std::move(flds));
    json::Value sys = json::Value::object();
    sys.set("ModifyAllData", modify_all_data);
    sys.set("ViewAllData", view_all_data);
    sys.set("ManageUsers", manage_users);
    sys.set("CustomizeApplication", customize_application);
    sys.set("ApiEnabled", api_enabled);
    sys.set("ExportReport", export_reports);
    sys.set("ViewSetup", view_setup);
    sys.set("BulkApi", bulk_api);
    v.set("systemPermissions", std::move(sys));
    return v;
}

Profile Profile::from_json(const json::Value& v) {
    Profile p;
    p.id = v.get_string("id");
    p.name = v.get_string("name");
    p.description = v.get_string("description");
    if (const json::Value* objs = v.find("objectPermissions"); objs && objs->is_object()) {
        for (const auto& [obj, perms] : objs->as_object()) p.objects[obj] = perms_from_json(perms);
    }
    if (const json::Value* d = v.find("defaultObjectPermissions"); d && d->is_object()) {
        p.default_object = perms_from_json(*d);
    }
    if (const json::Value* flds = v.find("fieldPermissions"); flds && flds->is_object()) {
        for (const auto& [name_, fp] : flds->as_object()) {
            FieldPermission perm;
            perm.read = fp.get_bool("read", true);
            perm.edit = perm.read && fp.get_bool("edit", true);
            p.fields[name_] = perm;
        }
    }
    const json::Value& sys = v.at("systemPermissions");
    p.modify_all_data = sys.get_bool("ModifyAllData");
    p.view_all_data = sys.get_bool("ViewAllData") || p.modify_all_data;
    p.manage_users = sys.get_bool("ManageUsers");
    p.customize_application = sys.get_bool("CustomizeApplication");
    p.api_enabled = sys.get_bool("ApiEnabled", true);
    p.export_reports = sys.get_bool("ExportReport");
    p.view_setup = sys.get_bool("ViewSetup");
    p.bulk_api = sys.get_bool("BulkApi");
    return p;
}

std::vector<Profile> default_profiles() {
    std::vector<Profile> out;

    Profile admin;
    admin.id = "00e000000000001AAA";
    admin.name = "System Administrator";
    admin.description = "Full access to data and setup";
    admin.default_object = ObjectPermissions::full();
    admin.modify_all_data = true;
    admin.view_all_data = true;
    admin.manage_users = true;
    admin.customize_application = true;
    admin.export_reports = true;
    admin.view_setup = true;
    admin.bulk_api = true;
    out.push_back(admin);

    Profile standard;
    standard.id = "00e000000000002AAA";
    standard.name = "Standard User";
    standard.description = "Works with the records they own or that are shared with them";
    for (const char* o : kSalesObjects) standard.objects[o] = ObjectPermissions::crud();
    standard.objects["Product2"] = ObjectPermissions::read_only();
    standard.objects["Campaign"] = ObjectPermissions::read_only();
    standard.export_reports = true;
    out.push_back(standard);

    Profile readonly;
    readonly.id = "00e000000000003AAA";
    readonly.name = "Read Only";
    readonly.description = "Can view records shared with them";
    for (const char* o : kSalesObjects) readonly.objects[o] = ObjectPermissions::read_only();
    out.push_back(readonly);

    Profile marketing;
    marketing.id = "00e000000000004AAA";
    marketing.name = "Marketing User";
    marketing.description = "Standard User plus campaigns and lead import";
    marketing.objects = standard.objects;
    marketing.objects["Campaign"] = ObjectPermissions::crud();
    marketing.export_reports = true;
    marketing.bulk_api = true;
    out.push_back(marketing);

    Profile integration;
    integration.id = "00e000000000005AAA";
    integration.name = "Integration User";
    integration.description = "API-only access for system integrations";
    for (const char* o : kSalesObjects) {
        ObjectPermissions p = ObjectPermissions::crud();
        p.view_all = true;
        integration.objects[o] = p;
    }
    integration.bulk_api = true;
    out.push_back(integration);

    return out;
}

const Profile* ProfileStore::find(const std::string& id) const {
    for (const Profile& p : profiles_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const Profile* ProfileStore::find_by_name(const std::string& name) const {
    for (const Profile& p : profiles_) {
        if (str::iequals(p.name, name)) return &p;
    }
    return nullptr;
}

Profile* ProfileStore::find_mut(const std::string& id) {
    for (Profile& p : profiles_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

void ProfileStore::put(Profile p) {
    for (Profile& existing : profiles_) {
        if (existing.id == p.id) {
            existing = std::move(p);
            return;
        }
    }
    profiles_.push_back(std::move(p));
}

json::Value ProfileStore::to_json() const {
    json::Value arr = json::Value::array();
    for (const Profile& p : profiles_) arr.push_back(p.to_json());
    return arr;
}

void ProfileStore::load_json(const json::Value& v) {
    profiles_.clear();
    if (!v.is_array()) return;
    for (const json::Value& p : v.as_array()) profiles_.push_back(Profile::from_json(p));
}

}  // namespace tcrm::security
