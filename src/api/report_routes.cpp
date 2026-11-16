// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <shared_mutex>

#include "api/context.hpp"
#include "services/reports.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

services::ReportDef find_report(const engine::Principal& who, const std::string& id) {
    std::shared_lock<std::shared_mutex> lock(who.org->mutex());
    for (const services::ReportDef& d : services::load_reports(*who.org)) {
        if (d.id != id) continue;
        if (!d.is_public && d.owner_id != who.user_id && !who.profile.modify_all_data) break;
        return d;
    }
    throw errors::not_found("No such report: " + id);
}

void touch_last_run(engine::Org& org, const std::string& id) {
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    std::vector<services::ReportDef> reports = services::load_reports(org);
    for (services::ReportDef& d : reports) {
        if (d.id == id) d.last_run_at = timeutil::now_ms();
    }
    services::store_reports(org, reports);
}

}  // namespace

void register_report_routes(http::Router& r, ApiContext& ctx) {
    const std::string base = "/services/data/v1/analytics/reports";

    r.get(base, authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value records = json::Value::array();
              for (const services::ReportDef& d : services::load_reports(*who.org)) {
                  if (!d.is_public && d.owner_id != who.user_id && !who.profile.modify_all_data) continue;
                  records.push_back(d.to_json());
              }
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    r.post(base, authed(ctx, [](http::Request& req) {
               const engine::Principal& who = principal(req);
               services::ReportDef def = services::ReportDef::from_json(body_object(req));
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               services::validate_report(org, def);
               def.id = new_config_id(org, "00O");
               def.owner_id = who.user_id;
               def.created_at = timeutil::now_ms();
               std::vector<services::ReportDef> reports = services::load_reports(org);
               reports.push_back(def);
               services::store_reports(org, reports);
               return json_response(201, def.to_json());
           }));

    r.get(base + "/{id}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              return json_response(200, find_report(who, req.path_param("id")).to_json());
          }));

    r.del(base + "/{id}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              std::vector<services::ReportDef> reports = services::load_reports(org);
              auto it = std::find_if(reports.begin(), reports.end(),
                                     [&](const services::ReportDef& d) { return d.id == req.path_param("id"); });
              if (it == reports.end()) throw errors::not_found();
              if (it->owner_id != who.user_id && !who.profile.modify_all_data) throw errors::forbidden();
              reports.erase(it);
              services::store_reports(org, reports);
              return http::Response::no_content();
          }));

    r.post(base + "/{id}/run", authed(ctx, [](http::Request& req) {
               const engine::Principal& who = principal(req);
               const services::ReportDef def = find_report(who, req.path_param("id"));
               json::Value run = services::run_report(who, def);
               touch_last_run(*who.org, def.id);
               if (req.query_param("format") == "csv") {
                   require(who.profile.export_reports || who.profile.modify_all_data, "export reports");
                   http::Response resp = http::Response::text(200, services::report_to_csv(def, run), "text/csv");
                   resp.headers.set("Content-Disposition", "attachment; filename=\"report-" + def.id + ".csv\"");
                   return resp;
               }
               return json_response(200, run);
           }));
}

}  // namespace tcrm::api
