// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "json/json.hpp"

namespace tcrm::security {

struct ObjectPermissions {
    bool read = false;
    bool create = false;
    bool edit = false;
    bool del = false;
    bool view_all = false;
    bool modify_all = false;

    static ObjectPermissions full() { return {true, true, true, true, true, true}; }
    static ObjectPermissions crud() { return {true, true, true, true, false, false}; }
    static ObjectPermissions read_only() { return {true, false, false, false, false, false}; }
};

struct FieldPermission {
    bool read = true;
    bool edit = true;
};

/// What a user may do: object CRUD, field-level security and system
/// permissions. Every user has exactly one profile.
struct Profile {
    std::string id;
    std::string name;
    std::string description;

    std::map<std::string, ObjectPermissions> objects;  // by object API name
    std::map<std::string, FieldPermission> fields;     // "Object.Field"; absent = read and edit

    ObjectPermissions default_object;  // for objects not listed (e.g. new custom objects)

    bool modify_all_data = false;
    bool view_all_data = false;
    bool manage_users = false;
    bool customize_application = false;
    bool api_enabled = true;
    bool export_reports = false;
    bool view_setup = false;
    bool bulk_api = false;

    ObjectPermissions object_permissions(const std::string& object) const;
    FieldPermission field_permission(const std::string& object, const std::string& field) const;

    json::Value to_json() const;
    static Profile from_json(const json::Value& v);
};

/// The profiles every new org starts with.
std::vector<Profile> default_profiles();

class ProfileStore {
public:
    const Profile* find(const std::string& id) const;
    const Profile* find_by_name(const std::string& name) const;
    Profile* find_mut(const std::string& id);
    void put(Profile p);
    const std::vector<Profile>& all() const { return profiles_; }

    json::Value to_json() const;
    void load_json(const json::Value& v);

private:
    std::vector<Profile> profiles_;
};

}  // namespace tcrm::security
