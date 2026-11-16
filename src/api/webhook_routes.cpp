// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <shared_mutex>

#include "api/context.hpp"
#include "util/crypto.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

void require_webhook_admin(const engine::Principal& who) {
    require(who.profile.customize_application || who.profile.modify_all_data, "manage webhooks");
}

}  // namespace

void register_webhook_routes(http::Router& r, ApiContext& ctx) {
    const std::string base = "/services/data/v1/webhooks";

    r.get(base, authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              require_webhook_admin(who);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value records = json::Value::array();
              for (const services::Webhook& w : services::load_webhooks(*who.org)) records.push_back(w.to_json(false));
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    r.post(base, authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               require_webhook_admin(who);
               services::Webhook w = services::Webhook::from_json(body_object(req));
               services::validate_webhook(w);
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               w.id = new_config_id(org, "0Wh");
               w.created_at = timeutil::now_ms();
               if (w.secret.empty()) w.secret = crypto::random_token(24);
               std::vector<services::Webhook> hooks = services::load_webhooks(org);
               hooks.push_back(w);
               services::store_webhooks(org, hooks);
               audit(ctx, who, "Webhooks", "Created webhook", w.name + " -> " + w.url);
               return json_response(201, w.to_json(true));
           }));

    r.del(base + "/{id}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              require_webhook_admin(who);
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              std::vector<services::Webhook> hooks = services::load_webhooks(org);
              const size_t before = hooks.size();
              hooks.erase(std::remove_if(hooks.begin(), hooks.end(),
                                         [&](const services::Webhook& w) { return w.id == req.path_param("id"); }),
                          hooks.end());
              if (hooks.size() == before) throw errors::not_found();
              services::store_webhooks(org, hooks);
              return http::Response::no_content();
          }));

    // Sends a sample payload so admins can check their endpoint.
    r.post(base + "/{id}/test", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               require_webhook_admin(who);
               services::Webhook hook;
               {
                   std::shared_lock<std::shared_mutex> lock(who.org->mutex());
                   bool found = false;
                   for (const services::Webhook& w : services::load_webhooks(*who.org)) {
                       if (w.id == req.path_param("id")) {
                           hook = w;
                           found = true;
                       }
                   }
                   if (!found) throw errors::not_found();
               }
               json::Value payload = json::Value::object();
               payload.set("event", "test");
               payload.set("message", "This is a test delivery from TropiCRM");
               ctx.webhooks.enqueue(who.org->id(), hook, payload);
               json::Value v = json::Value::object();
               v.set("queued", true);
               return json_response(202, v);
           }));

    r.get(base + "/deliveries", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              require_webhook_admin(who);
              json::Value records = json::Value::array();
              for (const services::DeliveryRecord& d : ctx.webhooks.recent(who.org->id(), 100)) {
                  records.push_back(d.to_json());
              }
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));
}

}  // namespace tcrm::api
