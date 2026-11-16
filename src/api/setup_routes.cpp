// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <shared_mutex>

#include "api/context.hpp"
#include "engine/records.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

struct Check {
    std::string id;
    std::string label;
    std::string status;  // Pass, Warn, Fail
    std::string detail;
};

json::Value check_json(const Check& c) {
    json::Value v = json::Value::object();
    v.set("id", c.id);
    v.set("label", c.label);
    v.set("status", c.status);
    v.set("detail", c.detail);
    return v;
}

std::vector<Check> run_checks(const ApiContext& ctx, const engine::Org& org) {
    std::vector<Check> out;
    const int64_t now = timeutil::now_ms();

    const int64_t iterations = ctx.config.get_int("auth.pbkdf2_iterations", 100000);
    out.push_back({"password_hashing", "Password hashing strength", iterations >= 100000 ? "Pass" : "Fail",
                   std::to_string(iterations) + " PBKDF2 iterations"});

    const int64_t ttl = ctx.config.get_int("auth.token_ttl_seconds", 7200);
    out.push_back({"session_timeout", "Session timeout", ttl <= 12 * 3600 ? "Pass" : "Warn",
                   "Access tokens last " + std::to_string(ttl / 60) + " minutes"});

    int admins = 0;
    int stale = 0;
    int old_keys = 0;
    for (const auth::User& u : org.users.all()) {
        if (!u.active) continue;
        const security::Profile* p = org.profiles.find(u.profile_id);
        if (p != nullptr && p->modify_all_data) ++admins;
        const int64_t last = u.last_login_at != 0 ? u.last_login_at : u.created_at;
        if (now - last > 90 * timeutil::kMsPerDay) ++stale;
        for (const auth::ApiKey& k : u.api_keys) {
            if (now - k.created_at > 365 * timeutil::kMsPerDay) ++old_keys;
        }
    }
    out.push_back({"admin_count", "Number of administrators", admins <= 3 ? "Pass" : "Warn",
                   std::to_string(admins) + " active users can modify all data"});
    out.push_back({"stale_users", "Inactive accounts", stale == 0 ? "Pass" : "Warn",
                   std::to_string(stale) + " active users haven't logged in for 90 days"});
    out.push_back({"api_key_age", "API key rotation", old_keys == 0 ? "Pass" : "Warn",
                   std::to_string(old_keys) + " API keys are older than a year"});

    for (const char* object : {"Account", "Opportunity", "Case"}) {
        const security::OrgDefault d = org.sharing.default_for(object);
        out.push_back({std::string("sharing_") + object, std::string(object) + " default access",
                       d == security::OrgDefault::PublicReadWrite ? "Warn" : "Pass",
                       std::string("Organization-wide default is ") + security::org_default_name(d)});
    }

    int active_webhooks = 0;
    if (const json::Value* hooks = org.db.meta("webhooks"); hooks && hooks->is_array()) {
        for (const json::Value& h : hooks->as_array()) active_webhooks += h.get_bool("active", true) ? 1 : 0;
    }
    out.push_back({"webhooks", "Outbound webhooks", active_webhooks <= 10 ? "Pass" : "Warn",
                   std::to_string(active_webhooks) + " active webhooks"});
    return out;
}

}  // namespace

void register_setup_routes(http::Router& r, ApiContext& ctx) {
    r.get("/services/data/v1/tooling/healthCheck", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.view_setup || who.profile.customize_application, "run the health check");
              std::vector<Check> checks;
              {
                  std::shared_lock<std::shared_mutex> lock(who.org->mutex());
                  checks = run_checks(ctx, *who.org);
              }
              int passed = 0;
              json::Value items = json::Value::array();
              for (const Check& c : checks) {
                  passed += c.status == "Pass" ? 1 : 0;
                  items.push_back(check_json(c));
              }
              json::Value v = json::Value::object();
              v.set("score", checks.empty() ? 100 : passed * 100 / static_cast<int>(checks.size()));
              v.set("checks", std::move(items));
              return json_response(200, v);
          }));

    r.get("/services/data/v1/recent", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              const std::vector<std::string> ids = ctx.recent.list(who.org->id(), who.user_id);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              engine::AccessChecker access(*who.org, who);
              json::Value records = json::Value::array();
              for (const std::string& id : ids) {
                  const meta::ObjectDef* obj = nullptr;
                  std::optional<Record> r = engine::load_record(*who.org, id, &obj);
                  if (!r || r->is_deleted() || !access.can_read(*obj, *r)) continue;
                  json::Value item = json::Value::object();
                  item.set("attributes", engine::record_attributes(obj->name, id));
                  item.set("Id", id);
                  item.set("Name", engine::record_name(*obj, *r));
                  records.push_back(std::move(item));
              }
              return json_response(200, records);
          }));
}

}  // namespace tcrm::api
