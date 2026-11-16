// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "auth/user.hpp"

#include "model/errors.hpp"
#include "util/crypto.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::auth {

std::string User::full_name() const {
    if (first_name.empty()) return last_name;
    if (last_name.empty()) return first_name;
    return first_name + " " + last_name;
}

json::Value User::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("username", username);
    v.set("firstName", first_name);
    v.set("lastName", last_name);
    v.set("email", email);
    v.set("title", title);
    v.set("department", department);
    v.set("profileId", profile_id);
    v.set("roleId", role_id);
    v.set("managerId", manager_id);
    v.set("timezone", timezone);
    v.set("locale", locale);
    v.set("active", active);
    v.set("passwordHash", password_hash);
    v.set("passwordSalt", password_salt);
    v.set("passwordIterations", static_cast<int64_t>(password_iterations));
    v.set("passwordChangedAt", password_changed_at);
    v.set("failedLogins", failed_logins);
    v.set("lockedUntil", locked_until);
    v.set("lastLoginAt", last_login_at);
    v.set("resetTokenHash", reset_token_hash);
    v.set("resetExpiresAt", reset_expires_at);
    v.set("createdAt", created_at);
    json::Value keys = json::Value::array();
    for (const ApiKey& k : api_keys) {
        json::Value jk = json::Value::object();
        jk.set("id", k.id);
        jk.set("prefix", k.prefix);
        jk.set("hash", k.hash);
        jk.set("label", k.label);
        jk.set("createdAt", k.created_at);
        jk.set("lastUsedAt", k.last_used_at);
        keys.push_back(std::move(jk));
    }
    v.set("apiKeys", std::move(keys));
    return v;
}

User User::from_json(const json::Value& v) {
    User u;
    u.id = v.get_string("id");
    u.username = v.get_string("username");
    u.first_name = v.get_string("firstName");
    u.last_name = v.get_string("lastName");
    u.email = v.get_string("email");
    u.title = v.get_string("title");
    u.department = v.get_string("department");
    u.profile_id = v.get_string("profileId");
    u.role_id = v.get_string("roleId");
    u.manager_id = v.get_string("managerId");
    u.timezone = v.get_string("timezone", "America/New_York");
    u.locale = v.get_string("locale", "en_US");
    u.active = v.get_bool("active", true);
    u.password_hash = v.get_string("passwordHash");
    u.password_salt = v.get_string("passwordSalt");
    u.password_iterations = static_cast<uint32_t>(v.get_int("passwordIterations", 0));
    u.password_changed_at = v.get_int("passwordChangedAt");
    u.failed_logins = static_cast<int>(v.get_int("failedLogins"));
    u.locked_until = v.get_int("lockedUntil");
    u.last_login_at = v.get_int("lastLoginAt");
    u.reset_token_hash = v.get_string("resetTokenHash");
    u.reset_expires_at = v.get_int("resetExpiresAt");
    u.created_at = v.get_int("createdAt");
    if (const json::Value* keys = v.find("apiKeys"); keys && keys->is_array()) {
        for (const json::Value& jk : keys->as_array()) {
            ApiKey k;
            k.id = jk.get_string("id");
            k.prefix = jk.get_string("prefix");
            k.hash = jk.get_string("hash");
            k.label = jk.get_string("label");
            k.created_at = jk.get_int("createdAt");
            k.last_used_at = jk.get_int("lastUsedAt");
            u.api_keys.push_back(std::move(k));
        }
    }
    return u;
}

void set_password(User* user, const std::string& password, uint32_t iterations) {
    const std::string salt = crypto::random_bytes(16);
    user->password_salt = crypto::to_hex(salt);
    user->password_iterations = iterations;
    user->password_hash = crypto::to_hex(crypto::pbkdf2_sha256(password, salt, iterations, 32));
    user->password_changed_at = timeutil::now_ms();
}

bool check_password(const User& user, const std::string& password) {
    if (user.password_hash.empty() || user.password_iterations == 0) return false;
    std::string salt;
    if (!crypto::from_hex(user.password_salt, &salt)) return false;
    const std::string computed =
        crypto::to_hex(crypto::pbkdf2_sha256(password, salt, user.password_iterations, 32));
    return crypto::constant_time_equals(computed, user.password_hash);
}

bool password_meets_policy(const std::string& password, std::string* reason) {
    if (password.size() < 8) {
        if (reason) *reason = "Passwords must be at least 8 characters";
        return false;
    }
    bool letter = false, digit = false;
    for (char c : password) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letter = true;
        if (c >= '0' && c <= '9') digit = true;
    }
    if (!letter || !digit) {
        if (reason) *reason = "Passwords must contain a letter and a digit";
        return false;
    }
    return true;
}

User* UserStore::find(const std::string& id) {
    for (User& u : users_) {
        if (u.id == id) return &u;
    }
    return nullptr;
}

const User* UserStore::find(const std::string& id) const {
    for (const User& u : users_) {
        if (u.id == id) return &u;
    }
    return nullptr;
}

User* UserStore::find_by_username(const std::string& username) {
    for (User& u : users_) {
        if (str::iequals(u.username, username)) return &u;
    }
    return nullptr;
}

const User* UserStore::find_by_username(const std::string& username) const {
    for (const User& u : users_) {
        if (str::iequals(u.username, username)) return &u;
    }
    return nullptr;
}

void UserStore::add(User user) {
    if (find_by_username(user.username) != nullptr) {
        throw errors::bad_request("DUPLICATE_USERNAME", "Username " + user.username + " is taken", {"username"});
    }
    user.username = str::to_lower(user.username);
    users_.push_back(std::move(user));
}

json::Value UserStore::to_json() const {
    json::Value arr = json::Value::array();
    for (const User& u : users_) arr.push_back(u.to_json());
    return arr;
}

void UserStore::load_json(const json::Value& v) {
    users_.clear();
    if (!v.is_array()) return;
    for (const json::Value& u : v.as_array()) users_.push_back(User::from_json(u));
}

}  // namespace tcrm::auth
