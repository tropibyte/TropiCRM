// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <set>
#include <shared_mutex>

#include "api/context.hpp"
#include "meta/codec.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

const std::set<std::string> kAdminOnlyFields = {"username", "profileId", "profileName", "roleId", "managerId",
                                                "active"};

/// Applies user attributes from a request body. Field names are matched
/// case-insensitively, like everywhere else in the API.
void apply_user_fields(engine::Org& org, const json::Value& body, auth::User* u) {
    for (const auto& [key, v] : body.as_object()) {
        if (!v.is_string() && !v.is_bool() && !v.is_null()) continue;
        const std::string s = v.is_string() ? v.as_string() : std::string();
        if (str::iequals(key, "firstName")) u->first_name = s;
        else if (str::iequals(key, "lastName")) u->last_name = s;
        else if (str::iequals(key, "email")) {
            if (!s.empty() && !meta::is_valid_email(s)) throw errors::invalid_field("email", "Invalid email address");
            u->email = s;
        } else if (str::iequals(key, "title")) u->title = s;
        else if (str::iequals(key, "department")) u->department = s;
        else if (str::iequals(key, "locale")) u->locale = s;
        else if (str::iequals(key, "timezone")) {
            timeutil::TimeZone tz;
            if (!timeutil::TimeZone::lookup(s, &tz)) throw errors::invalid_field("timezone", "Unknown time zone");
            u->timezone = tz.name();
        } else if (str::iequals(key, "profileId")) {
            if (org.profiles.find(s) == nullptr) throw errors::invalid_field("profileId", "No such profile");
            u->profile_id = s;
        } else if (str::iequals(key, "profileName")) {
            const security::Profile* p = org.profiles.find_by_name(s);
            if (p == nullptr) throw errors::invalid_field("profileName", "No such profile");
            u->profile_id = p->id;
        } else if (str::iequals(key, "roleId")) {
            if (!s.empty() && org.roles.find(s) == nullptr) throw errors::invalid_field("roleId", "No such role");
            u->role_id = s;
        } else if (str::iequals(key, "managerId")) {
            if (!s.empty() && org.users.find(s) == nullptr) throw errors::invalid_field("managerId", "No such user");
            u->manager_id = s;
        } else if (str::iequals(key, "active")) {
            u->active = v.is_bool() && v.as_bool();
        }
    }
}

auth::User* find_user(engine::Org& org, const std::string& id) {
    auth::User* u = org.users.find(id);
    if (u == nullptr) throw errors::not_found("No such user: " + id);
    return u;
}

std::string resolve_me(const engine::Principal& who, const std::string& id) {
    return id == "me" ? who.user_id : id;
}

}  // namespace

void register_user_routes(http::Router& r, ApiContext& ctx) {
    const std::string base = "/services/data/v1/users";

    r.get(base, authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value records = json::Value::array();
              for (const auth::User& u : who.org->users.all()) {
                  if (!u.active && !who.profile.manage_users) continue;
                  records.push_back(u.to_json());
              }
              json::Value v = json::Value::object();
              v.set("totalSize", records.size());
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    r.get(base + "/{id}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              const auth::User* u = who.org->users.find(resolve_me(who, req.path_param("id")));
              if (u == nullptr) throw errors::not_found();
              return json_response(200, u->to_json());
          }));

    r.post(base, authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               require(who.profile.manage_users, "manage users");
               const json::Value body = body_object(req);
               engine::Org& org = *who.org;
               auth::User u;
               {
                   std::unique_lock<std::shared_mutex> lock(org.mutex());
                   u.id = org.new_id("005");
                   u.username = str::to_lower(str::trim(body.get_string("username")));
                   if (u.username.find('@') == std::string::npos) {
                       throw errors::invalid_field("username", "Usernames must look like email addresses");
                   }
                   u.profile_id = org.profiles.find_by_name("Standard User")->id;
                   u.timezone = org.settings.timezone;
                   u.created_at = timeutil::now_ms();
                   apply_user_fields(org, body, &u);
                   if (u.last_name.empty()) throw errors::invalid_field("lastName", "Last name is required");
                   const std::string password = body.get_string("password");
                   if (!password.empty()) {
                       std::string reason;
                       if (!auth::password_meets_policy(password, &reason)) throw errors::invalid_field("password", reason);
                       auth::set_password(&u, password, ctx.auth.settings().pbkdf2_iterations);
                   }
                   org.users.add(u);
                   org.save_users();
               }
               ctx.registry.index_user(u.username, org.id());
               audit(ctx, who, "Manage Users", "Created user", u.username);
               if (body.get_string("password").empty()) ctx.auth.forgot_password(u.username);
               json::Value v = json::Value::object();
               v.set("id", u.id);
               v.set("success", true);
               return json_response(201, v);
           }));

    r.patch(base + "/{id}", authed(ctx, [&ctx](http::Request& req) {
                const engine::Principal& who = principal(req);
                const std::string id = resolve_me(who, req.path_param("id"));
                const bool admin = who.profile.manage_users;
                if (!admin && id != who.user_id) throw errors::forbidden("You can only edit your own user");
                const json::Value body = body_object(req);
                if (!admin) {
                    for (const auto& [key, v] : body.as_object()) {
                        if (kAdminOnlyFields.count(key) != 0) {
                            throw errors::forbidden("Only administrators can change " + key);
                        }
                    }
                }
                engine::Org& org = *who.org;
                std::unique_lock<std::shared_mutex> lock(org.mutex());
                auth::User* u = find_user(org, id);
                auth::User updated = *u;
                apply_user_fields(org, body, &updated);
                if (updated.id == who.user_id && !updated.active) {
                    throw errors::bad_request("CANNOT_DEACTIVATE_SELF", "You can't deactivate your own user");
                }
                *u = updated;
                org.save_users();
                if (admin) audit(ctx, who, "Manage Users", "Changed user", u->username);
                return http::Response::no_content();
            }));

    r.post(base + "/{id}/password", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const std::string id = resolve_me(who, req.path_param("id"));
               const bool admin = who.profile.manage_users;
               if (!admin && id != who.user_id) throw errors::forbidden();
               const json::Value body = body_object(req);
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               auth::User* u = find_user(org, id);
               if (id == who.user_id && !auth::check_password(*u, body.get_string("currentPassword"))) {
                   throw errors::bad_request("INVALID_PASSWORD", "The current password is incorrect");
               }
               std::string reason;
               const std::string next = body.get_string("newPassword");
               if (!auth::password_meets_policy(next, &reason)) throw errors::invalid_field("newPassword", reason);
               auth::set_password(u, next, ctx.auth.settings().pbkdf2_iterations);
               org.save_users();
               ctx.tokens.revoke_refresh_for_user(u->id);
               return http::Response::no_content();
           }));

    r.get(base + "/me/apikeys", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              const auth::User* u = who.org->users.find(who.user_id);
              json::Value records = json::Value::array();
              for (const auth::ApiKey& k : u->api_keys) {
                  json::Value jk = json::Value::object();
                  jk.set("id", k.id);
                  jk.set("prefix", "tck_" + k.prefix);
                  jk.set("label", k.label);
                  jk.set("createdDate", timeutil::format_datetime(k.created_at));
                  if (k.last_used_at) jk.set("lastUsedDate", timeutil::format_datetime(k.last_used_at));
                  records.push_back(std::move(jk));
              }
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    r.post(base + "/me/apikeys", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const json::Value body = body_object(req);
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               auth::User* u = find_user(org, who.user_id);
               if (u->api_keys.size() >= 10) throw errors::bad_request("LIMIT_EXCEEDED", "At most 10 API keys per user");
               const std::string key = ctx.auth.create_api_key(org, u, body.get_string("label", "API key"));
               org.save_users();
               json::Value v = json::Value::object();
               v.set("id", u->api_keys.back().id);
               v.set("key", key);
               v.set("note", "Store this key now; it won't be shown again.");
               return json_response(201, v);
           }));

    r.del(base + "/me/apikeys/{keyId}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              auth::User* u = find_user(org, who.user_id);
              const size_t before = u->api_keys.size();
              u->api_keys.erase(std::remove_if(u->api_keys.begin(), u->api_keys.end(),
                                               [&](const auth::ApiKey& k) { return k.id == req.path_param("keyId"); }),
                                u->api_keys.end());
              if (u->api_keys.size() == before) throw errors::not_found();
              org.save_users();
              return http::Response::no_content();
          }));
}

}  // namespace tcrm::api
