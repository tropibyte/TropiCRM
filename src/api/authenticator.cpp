// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "api/authenticator.hpp"

#include <ctime>
#include <random>
#include <shared_mutex>

#include "model/errors.hpp"
#include "util/crypto.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

ApiError invalid_grant(const std::string& message) {
    return ApiError(400, "invalid_grant", message);
}

/// Short, typeable codes for password-reset emails.
std::string reset_code() {
    static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::mt19937 rng(static_cast<uint32_t>(std::time(nullptr)));
    std::string code;
    for (int i = 0; i < 10; ++i) code += kAlphabet[rng() % (sizeof kAlphabet - 1)];
    return code;
}

}  // namespace

std::string cookie_value(const http::Request& req, const std::string& name) {
    const std::string header = req.headers.get("Cookie");
    for (const std::string& part : str::split(header, ';')) {
        const std::string_view kv = str::trim(part);
        const size_t eq = kv.find('=');
        if (eq != std::string_view::npos && kv.substr(0, eq) == name) return std::string(kv.substr(eq + 1));
    }
    return {};
}

Authenticator::Authenticator(engine::OrgRegistry& registry, auth::TokenService& tokens, services::Outbox& outbox,
                             AuthSettings settings)
    : registry_(registry), tokens_(tokens), outbox_(outbox), settings_(std::move(settings)) {}

engine::Principal Authenticator::principal_for(const std::shared_ptr<engine::Org>& org, const auth::User& user) {
    engine::Principal p;
    p.org = org;
    p.user_id = user.id;
    p.username = user.username;
    p.role_id = user.role_id;
    p.timezone = user.timezone;
    if (const security::Profile* prof = org->profiles.find(user.profile_id)) p.profile = *prof;
    return p;
}

LoginResult Authenticator::issue(const std::shared_ptr<engine::Org>& org, const auth::User& user) {
    auth::TokenClaims claims;
    claims.org_id = org->id();
    claims.user_id = user.id;
    LoginResult r;
    r.access_token = tokens_.issue(claims);
    r.refresh_token = tokens_.issue_refresh(org->id(), user.id, settings_.refresh_ttl_days);
    r.org_id = org->id();
    r.user_id = user.id;
    r.issued_at = timeutil::now_ms();
    r.expires_in = tokens_.ttl_seconds();
    return r;
}

LoginResult Authenticator::password_login(const std::string& username, const std::string& password,
                                          const std::string& client_ip) {
    const int64_t now = timeutil::now_ms();
    {
        std::lock_guard<std::mutex> lock(mu_);
        Failures& f = failures_[username];
        if (f.locked_until > now) {
            throw invalid_grant("authentication failure: account locked, try again later");
        }
    }
    std::shared_ptr<engine::Org> org = registry_.find_by_username(username);
    bool ok = false;
    std::string reason = "authentication failure";
    LoginResult result;
    if (org != nullptr) {
        std::unique_lock<std::shared_mutex> lock(org->mutex());
        auth::User* user = org->users.find_by_username(username);
        if (user != nullptr && user->active && org->active() && auth::check_password(*user, password)) {
            const security::Profile* prof = org->profiles.find(user->profile_id);
            if (prof == nullptr || !prof->api_enabled) {
                reason = "API access is disabled for this user";
            } else {
                user->last_login_at = now;
                org->save_users();
                result = issue(org, *user);
                ok = true;
            }
        } else if (user != nullptr && !user->active) {
            reason = "authentication failure: user is inactive";
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (ok) {
        failures_.erase(username);
        log::info("auth", "login ", username, " from ", client_ip);
        return result;
    }
    Failures& f = failures_[username];
    if (++f.count >= settings_.max_failed_logins) {
        f.locked_until = now + settings_.lockout_minutes * timeutil::kMsPerMinute;
        f.count = 0;
        log::warn("auth", "locking ", username, " after repeated failures from ", client_ip);
    }
    throw invalid_grant(reason);
}

LoginResult Authenticator::refresh(const std::string& refresh_token) {
    auth::TokenService::RefreshEntry entry;
    if (!tokens_.redeem_refresh(refresh_token, &entry)) throw invalid_grant("expired access/refresh token");
    std::shared_ptr<engine::Org> org = registry_.find(entry.org_id);
    if (org == nullptr || !org->active()) throw invalid_grant("expired access/refresh token");
    std::shared_lock<std::shared_mutex> lock(org->mutex());
    const auth::User* user = org->users.find(entry.user_id);
    if (user == nullptr || !user->active) throw invalid_grant("inactive user");
    auth::TokenClaims claims;
    claims.org_id = org->id();
    claims.user_id = user->id;
    LoginResult r;
    r.access_token = tokens_.issue(claims);
    r.refresh_token = refresh_token;
    r.org_id = org->id();
    r.user_id = user->id;
    r.issued_at = timeutil::now_ms();
    r.expires_in = tokens_.ttl_seconds();
    return r;
}

void Authenticator::revoke(const std::string& token) {
    auth::TokenClaims claims;
    std::string reason;
    if (tokens_.verify(token, &claims, &reason)) {
        tokens_.revoke(claims.token_id);
        return;
    }
    auth::TokenService::RefreshEntry entry;
    if (tokens_.redeem_refresh(token, &entry)) tokens_.revoke_refresh_for_user(entry.user_id);
}

engine::Principal Authenticator::from_token(const std::string& token) const {
    auth::TokenClaims claims;
    std::string reason;
    if (!tokens_.verify(token, &claims, &reason)) throw errors::unauthorized("Session expired or invalid");
    std::shared_ptr<engine::Org> org = registry_.find(claims.org_id);
    if (org == nullptr || !org->active()) throw errors::unauthorized();
    std::shared_lock<std::shared_mutex> lock(org->mutex());
    const auth::User* user = org->users.find(claims.user_id);
    if (user == nullptr || !user->active) throw errors::unauthorized();
    engine::Principal p = principal_for(org, *user);
    p.token_id = claims.token_id;
    return p;
}

engine::Principal Authenticator::from_api_key(const std::string& key) const {
    // tck_<prefix>_<secret>
    const std::vector<std::string> parts = str::split(key, '_');
    if (parts.size() != 3 || parts[0] != "tck") throw errors::unauthorized("Malformed API key");
    const std::string hash = crypto::to_hex(crypto::sha256(key));
    for (const auto& org : registry_.all()) {
        std::unique_lock<std::shared_mutex> lock(org->mutex());
        for (auth::User& u : org->users.all()) {
            for (auth::ApiKey& k : u.api_keys) {
                if (k.prefix != parts[1]) continue;
                if (!crypto::constant_time_equals(k.hash, hash) || !u.active || !org->active()) {
                    throw errors::unauthorized("Invalid API key");
                }
                k.last_used_at = timeutil::now_ms();
                engine::Principal p = principal_for(org, u);
                p.via_api_key = true;
                return p;
            }
        }
    }
    throw errors::unauthorized("Invalid API key");
}

engine::Principal Authenticator::authenticate(const http::Request& req) const {
    const std::string header = req.headers.get("Authorization");
    if (!header.empty()) {
        const size_t sp = header.find(' ');
        const std::string scheme = header.substr(0, sp);
        const std::string credential = sp == std::string::npos ? std::string() : std::string(str::trim(header.substr(sp + 1)));
        if (str::iequals(scheme, "Bearer") || str::iequals(scheme, "OAuth")) return from_token(credential);
        if (str::iequals(scheme, "ApiKey")) return from_api_key(credential);
        throw errors::unauthorized("Unsupported authorization scheme");
    }
    const std::string sid = cookie_value(req, "sid");
    if (!sid.empty()) return from_token(sid);
    throw errors::unauthorized("Session expired or invalid");
}

bool Authenticator::is_platform_admin(const http::Request& req) const {
    if (settings_.platform_admin_key_sha256.empty()) return false;
    const std::string key = req.headers.get("X-Platform-Admin-Key");
    if (key.empty()) return false;
    return crypto::constant_time_equals(crypto::to_hex(crypto::sha256(key)),
                                        str::to_lower(settings_.platform_admin_key_sha256));
}

void Authenticator::forgot_password(const std::string& username) {
    std::shared_ptr<engine::Org> org = registry_.find_by_username(username);
    if (org == nullptr) return;
    std::string to_address, user_id, org_id;
    const std::string code = reset_code();
    {
        std::unique_lock<std::shared_mutex> lock(org->mutex());
        auth::User* user = org->users.find_by_username(username);
        if (user == nullptr || !user->active) return;
        user->reset_token_hash = crypto::to_hex(crypto::sha256(code));
        user->reset_expires_at = timeutil::now_ms() + settings_.reset_ttl_minutes * timeutil::kMsPerMinute;
        org->save_users();
        to_address = user->email;
        user_id = user->id;
        org_id = org->id();
    }
    outbox_.send(org_id, user_id, to_address, "Reset your TropiCRM password",
                 "Use this code to reset your password: " + code + " (it expires in " +
                     std::to_string(settings_.reset_ttl_minutes) + " minutes)");
}

void Authenticator::reset_password(const std::string& username, const std::string& token,
                                   const std::string& new_password) {
    std::shared_ptr<engine::Org> org = registry_.find_by_username(username);
    if (org == nullptr) throw errors::bad_request("INVALID_TOKEN", "The reset code is invalid or has expired");
    std::unique_lock<std::shared_mutex> lock(org->mutex());
    auth::User* user = org->users.find_by_username(username);
    if (user == nullptr || user->reset_token_hash.empty() || user->reset_expires_at < timeutil::now_ms() ||
        !crypto::constant_time_equals(crypto::to_hex(crypto::sha256(token)), user->reset_token_hash)) {
        throw errors::bad_request("INVALID_TOKEN", "The reset code is invalid or has expired");
    }
    std::string reason;
    if (!auth::password_meets_policy(new_password, &reason)) throw errors::invalid_field("password", reason);
    auth::set_password(user, new_password, settings_.pbkdf2_iterations);
    user->reset_token_hash.clear();
    user->reset_expires_at = 0;
    org->save_users();
    tokens_.revoke_refresh_for_user(user->id);
}

std::string Authenticator::create_api_key(engine::Org& org, auth::User* user, const std::string& label) {
    (void)org;
    const std::string prefix = crypto::to_hex(crypto::random_bytes(4));
    const std::string key = "tck_" + prefix + "_" + crypto::to_hex(crypto::random_bytes(24));
    auth::ApiKey k;
    k.id = crypto::to_hex(crypto::random_bytes(8));
    k.prefix = prefix;
    k.hash = crypto::to_hex(crypto::sha256(key));
    k.label = label;
    k.created_at = timeutil::now_ms();
    user->api_keys.push_back(std::move(k));
    return key;
}

}  // namespace tcrm::api
