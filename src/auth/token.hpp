// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tcrm::auth {

struct TokenClaims {
    std::string org_id;
    std::string user_id;
    std::string token_id;  // unique per token; used for revocation
    std::string scope = "api";
    int64_t issued_at = 0;
    int64_t expires_at = 0;
};

/// Stateless access tokens: "v1.<base64url claims>.<base64url HMAC-SHA256>".
class TokenService {
public:
    explicit TokenService(std::string signing_key, int64_t ttl_seconds = 7200);

    /// Fills in token_id, issued_at and expires_at and returns the token.
    std::string issue(TokenClaims claims) const;

    /// Checks format, signature, expiry and revocation.
    bool verify(std::string_view token, TokenClaims* out, std::string* reason) const;

    void revoke(const std::string& token_id);

    int64_t ttl_seconds() const { return ttl_seconds_; }

    struct RefreshEntry {
        std::string org_id;
        std::string user_id;
        int64_t expires_at = 0;
    };

    /// Opaque refresh tokens, kept server-side by hash.
    std::string issue_refresh(const std::string& org_id, const std::string& user_id, int64_t ttl_days);
    bool redeem_refresh(const std::string& token, RefreshEntry* out);
    void revoke_refresh_for_user(const std::string& user_id);

private:
    std::string sign(std::string_view payload) const;

    std::string key_;
    int64_t ttl_seconds_;
    mutable std::mutex mu_;
    std::unordered_set<std::string> revoked_;
    std::unordered_map<std::string, RefreshEntry> refresh_;  // by hex SHA-256 of the token
};

}  // namespace tcrm::auth
