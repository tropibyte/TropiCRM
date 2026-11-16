// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json/json.hpp"

namespace tcrm::auth {

struct ApiKey {
    std::string id;
    std::string prefix;  // shown to the user; used to find the key
    std::string hash;    // hex SHA-256 of the full key
    std::string label;
    int64_t created_at = 0;
    int64_t last_used_at = 0;
};

struct User {
    std::string id;
    std::string username;  // unique across the platform, stored lowercase
    std::string first_name;
    std::string last_name;
    std::string email;
    std::string title;
    std::string department;
    std::string profile_id;
    std::string role_id;
    std::string manager_id;
    std::string timezone = "America/New_York";
    std::string locale = "en_US";
    bool active = true;

    std::string password_hash;  // hex PBKDF2-HMAC-SHA256
    std::string password_salt;  // hex
    uint32_t password_iterations = 0;
    int64_t password_changed_at = 0;

    int failed_logins = 0;
    int64_t locked_until = 0;
    int64_t last_login_at = 0;

    std::string reset_token_hash;
    int64_t reset_expires_at = 0;

    std::vector<ApiKey> api_keys;

    int64_t created_at = 0;

    std::string full_name() const;

    json::Value to_json() const;
    static User from_json(const json::Value& v);
};

/// Hashes `password` with a fresh random salt and stores it on `user`.
void set_password(User* user, const std::string& password, uint32_t iterations);

/// Checks `password` against the stored hash.
bool check_password(const User& user, const std::string& password);

/// Minimum password rules: 8+ characters with a letter and a digit.
bool password_meets_policy(const std::string& password, std::string* reason);

/// The users of one org.
class UserStore {
public:
    User* find(const std::string& id);
    const User* find(const std::string& id) const;
    User* find_by_username(const std::string& username);
    const User* find_by_username(const std::string& username) const;

    /// Throws ApiError if the username is taken.
    void add(User user);

    std::vector<User>& all() { return users_; }
    const std::vector<User>& all() const { return users_; }
    size_t size() const { return users_.size(); }

    json::Value to_json() const;
    void load_json(const json::Value& v);

private:
    std::vector<User> users_;
};

}  // namespace tcrm::auth
