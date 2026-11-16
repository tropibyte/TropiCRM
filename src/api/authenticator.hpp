// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "auth/token.hpp"
#include "engine/access.hpp"
#include "engine/org.hpp"
#include "http/message.hpp"
#include "services/outbox.hpp"

namespace tcrm::api {

struct AuthSettings {
    int max_failed_logins = 5;
    int lockout_minutes = 15;
    int reset_ttl_minutes = 60;
    int refresh_ttl_days = 30;
    uint32_t pbkdf2_iterations = 100000;
    std::string platform_admin_key_sha256;  // hex; empty disables the platform admin API
};

struct LoginResult {
    std::string access_token;
    std::string refresh_token;
    std::string org_id;
    std::string user_id;
    int64_t issued_at = 0;
    int64_t expires_in = 0;
};

/// Turns credentials into principals: password and refresh-token logins,
/// bearer tokens, API keys and session cookies, plus password resets.
class Authenticator {
public:
    Authenticator(engine::OrgRegistry& registry, auth::TokenService& tokens, services::Outbox& outbox,
                  AuthSettings settings);

    /// Throws ApiError(400 invalid_grant) for bad credentials or a locked
    /// account.
    LoginResult password_login(const std::string& username, const std::string& password, const std::string& client_ip);
    LoginResult refresh(const std::string& refresh_token);
    void revoke(const std::string& token);

    /// Authenticates a request from its Authorization header or session
    /// cookie. Throws ApiError(401).
    engine::Principal authenticate(const http::Request& req) const;

    /// True when the request carries the platform admin key.
    bool is_platform_admin(const http::Request& req) const;

    /// Builds the principal for a user. The caller holds the org lock.
    static engine::Principal principal_for(const std::shared_ptr<engine::Org>& org, const auth::User& user);

    /// Starts a password reset; always looks successful to the caller.
    void forgot_password(const std::string& username);
    /// Completes one. Throws ApiError for a bad or expired token.
    void reset_password(const std::string& username, const std::string& token, const std::string& new_password);

    /// Creates an API key for the user and returns it (shown once).
    std::string create_api_key(engine::Org& org, auth::User* user, const std::string& label);

    const AuthSettings& settings() const { return settings_; }

private:
    struct Failures {
        int count = 0;
        int64_t locked_until = 0;
    };

    engine::Principal from_token(const std::string& token) const;
    engine::Principal from_api_key(const std::string& key) const;
    LoginResult issue(const std::shared_ptr<engine::Org>& org, const auth::User& user);

    engine::OrgRegistry& registry_;
    auth::TokenService& tokens_;
    services::Outbox& outbox_;
    AuthSettings settings_;
    std::mutex mu_;
    std::map<std::string, Failures> failures_;  // by username as submitted
};

/// The value of a cookie in the request, or "".
std::string cookie_value(const http::Request& req, const std::string& name);

}  // namespace tcrm::api
