// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <shared_mutex>

#include "api/context.hpp"
#include "engine/records.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"
#include "util/url.hpp"

namespace tcrm::api {
namespace {

/// OAuth endpoints accept form-encoded or JSON bodies.
url::Params oauth_params(const http::Request& req) {
    const std::string ctype = str::to_lower(req.headers.get("Content-Type"));
    if (ctype.find("application/json") != std::string::npos) {
        url::Params out;
        const json::Value body = body_object(req);
        for (const auto& [k, v] : body.as_object()) {
            if (v.is_string()) out.emplace_back(k, v.as_string());
        }
        return out;
    }
    url::Params out = url::parse_query(req.body);
    for (const auto& kv : req.query) out.push_back(kv);
    return out;
}

http::Response token_response(const LoginResult& r) {
    json::Value body = json::Value::object();
    body.set("access_token", r.access_token);
    body.set("refresh_token", r.refresh_token);
    body.set("token_type", "Bearer");
    body.set("instance_url", "");
    body.set("id", std::string("/services/oauth2/userinfo"));
    body.set("org_id", r.org_id);
    body.set("user_id", r.user_id);
    body.set("issued_at", std::to_string(r.issued_at));
    body.set("expires_in", r.expires_in);
    http::Response resp = json_response(200, body);
    resp.headers.add("Set-Cookie", "sid=" + r.access_token + "; Path=/; HttpOnly");
    resp.headers.set("Cache-Control", "no-store");
    return resp;
}

}  // namespace

void register_auth_routes(http::Router& r, ApiContext& ctx) {
    r.post("/services/oauth2/token", [&ctx](http::Request& req) {
        const url::Params p = oauth_params(req);
        const std::string grant = url::param(p, "grant_type");
        if (grant == "password") {
            const std::string username = url::param(p, "username");
            const std::string password = url::param(p, "password");
            if (username.empty() || password.empty()) {
                throw ApiError(400, "invalid_request", "username and password are required");
            }
            return token_response(ctx.auth.password_login(username, password, req.remote_addr));
        }
        if (grant == "refresh_token") {
            const std::string token = url::param(p, "refresh_token");
            if (token.empty()) throw ApiError(400, "invalid_request", "refresh_token is required");
            return token_response(ctx.auth.refresh(token));
        }
        throw ApiError(400, "unsupported_grant_type", "grant type not supported");
    });

    r.post("/services/oauth2/revoke", [&ctx](http::Request& req) {
        const url::Params p = oauth_params(req);
        const std::string token = url::param(p, "token");
        if (!token.empty()) ctx.auth.revoke(token);
        http::Response resp;
        resp.status = 200;
        resp.headers.add("Set-Cookie", "sid=; Path=/; Max-Age=0");
        return resp;
    });

    r.get("/services/oauth2/userinfo", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              const auth::User* u = who.org->users.find(who.user_id);
              json::Value v = json::Value::object();
              v.set("user_id", who.user_id);
              v.set("organization_id", who.org->id());
              v.set("organization_name", who.org->name());
              v.set("preferred_username", who.username);
              v.set("name", u ? u->full_name() : who.username);
              v.set("email", u ? u->email : "");
              v.set("profile", who.profile.name);
              v.set("zoneinfo", who.timezone);
              v.set("locale", u ? u->locale : "en_US");
              return json_response(200, v);
          }));

    r.post("/services/auth/password/forgot", [&ctx](http::Request& req) {
        const json::Value body = body_object(req);
        const std::string username = body.get_string("username");
        if (!username.empty()) ctx.auth.forgot_password(username);
        json::Value v = json::Value::object();
        v.set("message", "If that account exists, we've emailed a reset code to its owner.");
        return json_response(200, v);
    });

    r.post("/services/auth/password/reset", [&ctx](http::Request& req) {
        const json::Value body = body_object(req);
        ctx.auth.reset_password(body.get_string("username"), body.get_string("code"), body.get_string("newPassword"));
        json::Value v = json::Value::object();
        v.set("success", true);
        return json_response(200, v);
    });

    // Self-service trial signup: creates a new org with the caller as its admin.
    r.post("/services/signup", [&ctx](http::Request& req) {
        if (!ctx.config.get_bool("platform.signup_enabled", true)) {
            throw ApiError(403, "SIGNUP_DISABLED", "Self-service signup is disabled on this server");
        }
        const json::Value body = body_object(req);
        engine::OrgRegistry::NewOrg spec;
        spec.name = body.get_string("company");
        spec.admin_username = body.get_string("username");
        spec.admin_first_name = body.get_string("firstName");
        spec.admin_last_name = body.get_string("lastName", "Admin");
        spec.admin_email = body.get_string("email", spec.admin_username);
        spec.admin_password = body.get_string("password");
        spec.timezone = body.get_string("timezone");
        spec.currency = body.get_string("currency");
        auto org = ctx.registry.create(spec);
        json::Value v = json::Value::object();
        v.set("orgId", org->id());
        v.set("username", spec.admin_username);
        v.set("loginUrl", "/services/oauth2/token");
        return json_response(201, v);
    });
}

}  // namespace tcrm::api
