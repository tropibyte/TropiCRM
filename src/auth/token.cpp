// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "auth/token.hpp"

#include "json/json.hpp"
#include "util/crypto.hpp"
#include "util/time.hpp"

namespace tcrm::auth {

TokenService::TokenService(std::string signing_key, int64_t ttl_seconds)
    : key_(std::move(signing_key)), ttl_seconds_(ttl_seconds) {}

std::string TokenService::sign(std::string_view payload) const {
    return crypto::base64url_encode(crypto::digest_bytes(crypto::hmac_sha256(key_, payload)));
}

std::string TokenService::issue(TokenClaims claims) const {
    const int64_t now = timeutil::now_ms();
    claims.token_id = crypto::random_token(12);
    claims.issued_at = now;
    claims.expires_at = now + ttl_seconds_ * 1000;
    json::Value body = json::Value::object();
    body.set("org", claims.org_id);
    body.set("sub", claims.user_id);
    body.set("jti", claims.token_id);
    body.set("scope", claims.scope);
    body.set("iat", claims.issued_at);
    body.set("exp", claims.expires_at);
    const std::string payload = "v1." + crypto::base64url_encode(body.dump());
    return payload + "." + sign(payload);
}

bool TokenService::verify(std::string_view token, TokenClaims* out, std::string* reason) const {
    const size_t last_dot = token.rfind('.');
    if (token.substr(0, 3) != "v1." || last_dot == std::string_view::npos || last_dot < 3) {
        if (reason) *reason = "malformed token";
        return false;
    }
    const std::string_view payload = token.substr(0, last_dot);
    const std::string_view signature = token.substr(last_dot + 1);
    if (sign(payload) != signature) {
        if (reason) *reason = "bad signature";
        return false;
    }
    std::string claims_json;
    if (!crypto::base64url_decode(payload.substr(3), &claims_json)) {
        if (reason) *reason = "malformed token";
        return false;
    }
    json::Value body;
    if (!json::try_parse(claims_json, &body) || !body.is_object()) {
        if (reason) *reason = "malformed token";
        return false;
    }
    TokenClaims c;
    c.org_id = body.get_string("org");
    c.user_id = body.get_string("sub");
    c.token_id = body.get_string("jti");
    c.scope = body.get_string("scope", "api");
    c.issued_at = body.get_int("iat");
    c.expires_at = body.get_int("exp");
    // exp is in seconds, as in JWT.
    if (c.expires_at < timeutil::now_ms() / 1000) {
        if (reason) *reason = "token expired";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (revoked_.count(c.token_id) != 0) {
            if (reason) *reason = "token revoked";
            return false;
        }
    }
    *out = std::move(c);
    return true;
}

void TokenService::revoke(const std::string& token_id) {
    std::lock_guard<std::mutex> lock(mu_);
    revoked_.insert(token_id);
}

std::string TokenService::issue_refresh(const std::string& org_id, const std::string& user_id, int64_t ttl_days) {
    const std::string token = "rt_" + crypto::random_token(32);
    RefreshEntry e;
    e.org_id = org_id;
    e.user_id = user_id;
    e.expires_at = timeutil::now_ms() + ttl_days * timeutil::kMsPerDay;
    std::lock_guard<std::mutex> lock(mu_);
    refresh_[crypto::to_hex(crypto::sha256(token))] = std::move(e);
    return token;
}

bool TokenService::redeem_refresh(const std::string& token, RefreshEntry* out) {
    const std::string key = crypto::to_hex(crypto::sha256(token));
    std::lock_guard<std::mutex> lock(mu_);
    auto it = refresh_.find(key);
    if (it == refresh_.end()) return false;
    if (it->second.expires_at < timeutil::now_ms()) {
        refresh_.erase(it);
        return false;
    }
    *out = it->second;
    return true;
}

void TokenService::revoke_refresh_for_user(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = refresh_.begin(); it != refresh_.end();) {
        if (it->second.user_id == user_id) {
            it = refresh_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace tcrm::auth
