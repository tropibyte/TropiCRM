// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <shared_mutex>

#include "api/context.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

void require_platform(ApiContext& ctx, const http::Request& req) {
    if (!ctx.auth.is_platform_admin(req)) throw ApiError(403, "INSUFFICIENT_ACCESS", "Platform administrator key required");
}

/// Support tooling is available to platform operators and to org admins
/// who can manage users.
bool support_access(ApiContext& ctx, http::Request& req) {
    if (ctx.auth.is_platform_admin(req)) return true;
    engine::Principal who = ctx.auth.authenticate(req);
    const bool allowed = who.profile.manage_users;
    req.auth = std::move(who);
    return allowed;
}

json::Value org_summary(engine::Org& org) {
    std::shared_lock<std::shared_mutex> lock(org.mutex());
    json::Value v = json::Value::object();
    v.set("id", org.id());
    v.set("name", org.name());
    v.set("active", org.active());
    v.set("createdDate", timeutil::format_datetime(org.created_at()));
    v.set("users", org.users.size());
    v.set("apiCallsToday", static_cast<int64_t>(org.usage.calls.load()));
    v.set("apiCallsPerDay", org.settings.api_calls_per_day);
    v.set("approximateBytes", static_cast<int64_t>(org.db.approximate_bytes()));
    v.set("logEntries", static_cast<int64_t>(org.db.log_entries()));
    return v;
}

std::shared_ptr<engine::Org> require_org(ApiContext& ctx, const std::string& id) {
    auto org = ctx.registry.find(id);
    if (org == nullptr) throw errors::not_found("No such org: " + id);
    return org;
}

}  // namespace

void register_admin_routes(http::Router& r, ApiContext& ctx) {
    const std::string base = "/services/admin/v1";

    r.get(base + "/orgs", [&ctx](http::Request& req) {
        require_platform(ctx, req);
        json::Value records = json::Value::array();
        for (const auto& org : ctx.registry.all()) records.push_back(org_summary(*org));
        json::Value v = json::Value::object();
        v.set("records", std::move(records));
        return json_response(200, v);
    });

    r.post(base + "/orgs", [&ctx](http::Request& req) {
        require_platform(ctx, req);
        const json::Value body = body_object(req);
        engine::OrgRegistry::NewOrg spec;
        spec.name = body.get_string("name");
        spec.admin_username = body.get_string("adminUsername");
        spec.admin_first_name = body.get_string("adminFirstName");
        spec.admin_last_name = body.get_string("adminLastName", "Admin");
        spec.admin_email = body.get_string("adminEmail", spec.admin_username);
        spec.admin_password = body.get_string("adminPassword");
        spec.timezone = body.get_string("timezone");
        spec.currency = body.get_string("currency");
        auto org = ctx.registry.create(spec);
        return json_response(201, org_summary(*org));
    });

    r.patch(base + "/orgs/{orgId}", [&ctx](http::Request& req) {
        require_platform(ctx, req);
        auto org = require_org(ctx, req.path_param("orgId"));
        const json::Value body = body_object(req);
        {
            std::unique_lock<std::shared_mutex> lock(org->mutex());
            if (body.contains("active")) org->set_active(body.get_bool("active"));
            if (body.contains("name")) org->set_name(body.get_string("name"));
            if (body.contains("apiCallsPerDay")) org->settings.api_calls_per_day = body.get_int("apiCallsPerDay");
            if (body.contains("storageMb")) org->settings.storage_mb = body.get_int("storageMb");
            org->save_settings();
        }
        return json_response(200, org_summary(*org));
    });

    r.get(base + "/orgs/{orgId}/users", [&ctx](http::Request& req) {
        if (!support_access(ctx, req)) throw errors::forbidden("Support tools require Manage Users");
        auto org = require_org(ctx, req.path_param("orgId"));
        std::shared_lock<std::shared_mutex> lock(org->mutex());
        json::Value records = json::Value::array();
        for (const auth::User& u : org->users.all()) {
            json::Value v = json::Value::object();
            v.set("id", u.id);
            v.set("username", u.username);
            v.set("name", u.full_name());
            v.set("email", u.email);
            v.set("active", u.active);
            const security::Profile* p = org->profiles.find(u.profile_id);
            v.set("profile", p ? p->name : "");
            v.set("lastLoginDate", u.last_login_at ? timeutil::format_datetime(u.last_login_at) : "");
            records.push_back(std::move(v));
        }
        json::Value v = json::Value::object();
        v.set("records", std::move(records));
        return json_response(200, v);
    });

    // Support: set a temporary password for a locked-out user.
    r.post(base + "/orgs/{orgId}/users/{userId}/password", [&ctx](http::Request& req) {
        if (!support_access(ctx, req)) throw errors::forbidden("Support tools require Manage Users");
        auto org = require_org(ctx, req.path_param("orgId"));
        const json::Value body = body_object(req);
        const std::string password = body.get_string("temporaryPassword");
        std::string reason;
        if (!auth::password_meets_policy(password, &reason)) throw errors::invalid_field("temporaryPassword", reason);
        std::unique_lock<std::shared_mutex> lock(org->mutex());
        auth::User* u = org->users.find(req.path_param("userId"));
        if (u == nullptr) throw errors::not_found("No such user");
        auth::set_password(u, password, ctx.auth.settings().pbkdf2_iterations);
        u->failed_logins = 0;
        u->locked_until = 0;
        org->save_users();
        ctx.tokens.revoke_refresh_for_user(u->id);
        json::Value v = json::Value::object();
        v.set("success", true);
        return json_response(200, v);
    });

    r.post(base + "/snapshot", [&ctx](http::Request& req) {
        require_platform(ctx, req);
        ctx.registry.snapshot_if_needed(0);
        json::Value v = json::Value::object();
        v.set("success", true);
        return json_response(200, v);
    });

    r.get(base + "/stats", [&ctx](http::Request& req) {
        require_platform(ctx, req);
        json::Value v = json::Value::object();
        v.set("orgs", ctx.registry.all().size());
        v.set("openQueryCursors", ctx.cursors.open_cursors());
        v.set("pendingWebhooks", ctx.webhooks.pending());
        v.set("describeCacheEntries", ctx.describe_cache.size());
        v.set("uptimeSeconds", (timeutil::now_ms() - ctx.started_at) / 1000);
        return json_response(200, v);
    });
}

}  // namespace tcrm::api
