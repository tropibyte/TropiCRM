// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <shared_mutex>

#include "api/context.hpp"
#include "engine/forecast.hpp"
#include "services/dashboards.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

services::Dashboard find_dashboard(const engine::Principal& who, const std::string& id) {
    std::shared_lock<std::shared_mutex> lock(who.org->mutex());
    for (const services::Dashboard& d : services::load_dashboards(*who.org)) {
        if (d.id == id) return d;
    }
    throw errors::not_found("No such dashboard: " + id);
}

}  // namespace

void register_analytics_routes(http::Router& r, ApiContext& ctx) {
    r.get("/services/data/v1/forecasts", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              auto [start, end] = engine::current_quarter(*who.org);
              if (req.has_query_param("start") && !timeutil::parse_date(req.query_param("start"), &start)) {
                  throw errors::invalid_field("start", "start must be YYYY-MM-DD");
              }
              if (req.has_query_param("end") && !timeutil::parse_date(req.query_param("end"), &end)) {
                  throw errors::invalid_field("end", "end must be YYYY-MM-DD");
              }
              if (end <= start || end - start > 731) throw errors::invalid_field("end", "The range must be 1 day to 2 years");
              {
                  std::shared_lock<std::shared_mutex> lock(who.org->mutex());
                  const meta::ObjectDef* opp = who.org->schema.object("Opportunity");
                  engine::AccessChecker(*who.org, who).require_object(*opp, engine::ObjectOp::Read);
              }
              json::Value v = engine::build_forecast(who, start, end).to_json();
              v.set("start", timeutil::format_date(start));
              v.set("end", timeutil::format_date(end));
              return json_response(200, v);
          }));

    const std::string base = "/services/data/v1/analytics/dashboards";

    r.get(base, authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value records = json::Value::array();
              for (const services::Dashboard& d : services::load_dashboards(*who.org)) records.push_back(d.to_json());
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    r.post(base, authed(ctx, [](http::Request& req) {
               const engine::Principal& who = principal(req);
               services::Dashboard d = services::Dashboard::from_json(body_object(req));
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               services::validate_dashboard(org, &d);
               d.id = new_config_id(org, "01Z");
               d.owner_id = who.user_id;
               d.created_at = timeutil::now_ms();
               std::vector<services::Dashboard> all = services::load_dashboards(org);
               all.push_back(d);
               services::store_dashboards(org, all);
               return json_response(201, d.to_json());
           }));

    r.get(base + "/{id}", authed(ctx, [](http::Request& req) {
              return json_response(200, find_dashboard(principal(req), req.path_param("id")).to_json());
          }));

    r.del(base + "/{id}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              std::vector<services::Dashboard> all = services::load_dashboards(org);
              auto it = std::find_if(all.begin(), all.end(),
                                     [&](const services::Dashboard& d) { return d.id == req.path_param("id"); });
              if (it == all.end()) throw errors::not_found();
              if (it->owner_id != who.user_id && !who.profile.modify_all_data) throw errors::forbidden();
              all.erase(it);
              services::store_dashboards(org, all);
              return http::Response::no_content();
          }));

    r.post(base + "/{id}/run", authed(ctx, [](http::Request& req) {
               const engine::Principal& who = principal(req);
               const services::Dashboard d = find_dashboard(who, req.path_param("id"));
               return json_response(200, services::run_dashboard(who, d));
           }));
}

}  // namespace tcrm::api
