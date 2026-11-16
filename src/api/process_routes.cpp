// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <shared_mutex>

#include "api/context.hpp"
#include "engine/merge.hpp"
#include "engine/records.hpp"
#include "services/templates.hpp"
#include "util/id.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::api {
namespace {

json::Value request_list(const std::vector<engine::ApprovalRequest>& requests) {
    json::Value records = json::Value::array();
    for (const auto& r : requests) records.push_back(r.to_json());
    json::Value v = json::Value::object();
    v.set("totalSize", records.size());
    v.set("records", std::move(records));
    return v;
}

const services::EmailTemplate& find_template(const std::vector<services::EmailTemplate>& all,
                                             const engine::Principal& who, const std::string& id) {
    for (const services::EmailTemplate& t : all) {
        if (t.id != id) continue;
        if (!t.is_public && t.owner_id != who.user_id && !who.profile.modify_all_data) break;
        return t;
    }
    throw errors::not_found("No such email template: " + id);
}

}  // namespace

void register_process_routes(http::Router& r, ApiContext& ctx) {
    // ---- approval processes (setup) -------------------------------------------
    r.get("/services/data/v1/tooling/approvalProcesses", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.view_setup || who.profile.customize_application, "view approval processes");
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value records = json::Value::array();
              for (const auto& p : engine::load_approval_processes(*who.org)) records.push_back(p.to_json());
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    r.post("/services/data/v1/tooling/approvalProcesses", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               require(who.profile.customize_application, "manage approval processes");
               engine::ApprovalProcess p = engine::ApprovalProcess::from_json(body_object(req));
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               engine::validate_approval_process(org, &p);
               p.id = new_config_id(org, "04a");
               std::vector<engine::ApprovalProcess> all = engine::load_approval_processes(org);
               all.push_back(p);
               engine::store_approval_processes(org, all);
               audit(ctx, who, "Approval Processes", "Created approval process", p.name);
               return json_response(201, p.to_json());
           }));

    r.del("/services/data/v1/tooling/approvalProcesses/{id}", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.customize_application, "manage approval processes");
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              std::vector<engine::ApprovalProcess> all = engine::load_approval_processes(org);
              const size_t before = all.size();
              all.erase(std::remove_if(all.begin(), all.end(),
                                       [&](const engine::ApprovalProcess& p) { return p.id == req.path_param("id"); }),
                        all.end());
              if (all.size() == before) throw errors::not_found();
              for (const auto& pending : engine::load_approval_requests(org)) {
                  if (pending.process_id == req.path_param("id") && pending.status == engine::ApprovalStatus::Pending) {
                      throw errors::bad_request("DELETE_FAILED", "The process has pending requests");
                  }
              }
              engine::store_approval_processes(org, all);
              audit(ctx, who, "Approval Processes", "Deleted approval process", req.path_param("id"));
              return http::Response::no_content();
          }));

    // ---- approval requests ------------------------------------------------------
    r.post("/services/data/v1/process/approvals", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const json::Value body = body_object(req);
               const engine::ApprovalRequest result =
                   ctx.approvals.submit(who, body.get_string("recordId"), body.get_string("comments"));
               return json_response(201, result.to_json());
           }));

    r.post("/services/data/v1/process/approvals/{id}", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const json::Value body = body_object(req);
               const engine::ApprovalRequest result =
                   ctx.approvals.act(who, req.path_param("id"), body.get_string("action"), body.get_string("comments"));
               return json_response(200, result.to_json());
           }));

    r.get("/services/data/v1/process/approvals", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              if (req.query_param("view") == "submitted") return json_response(200, request_list(ctx.approvals.submitted_by(who)));
              return json_response(200, request_list(ctx.approvals.pending_for(who)));
          }));

    // ---- email templates -----------------------------------------------------------
    r.get("/services/data/v1/emailTemplates", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              std::shared_lock<std::shared_mutex> lock(who.org->mutex());
              json::Value records = json::Value::array();
              for (const auto& t : services::load_templates(*who.org)) {
                  if (t.is_public || t.owner_id == who.user_id || who.profile.modify_all_data) records.push_back(t.to_json());
              }
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    r.post("/services/data/v1/emailTemplates", authed(ctx, [](http::Request& req) {
               const engine::Principal& who = principal(req);
               services::EmailTemplate t = services::EmailTemplate::from_json(body_object(req));
               engine::Org& org = *who.org;
               std::unique_lock<std::shared_mutex> lock(org.mutex());
               services::validate_template(org, &t);
               t.id = new_config_id(org, "00X");
               t.owner_id = who.user_id;
               t.created_at = timeutil::now_ms();
               std::vector<services::EmailTemplate> all = services::load_templates(org);
               all.push_back(t);
               services::store_templates(org, all);
               return json_response(201, t.to_json());
           }));

    r.del("/services/data/v1/emailTemplates/{id}", authed(ctx, [](http::Request& req) {
              const engine::Principal& who = principal(req);
              engine::Org& org = *who.org;
              std::unique_lock<std::shared_mutex> lock(org.mutex());
              std::vector<services::EmailTemplate> all = services::load_templates(org);
              auto it = std::find_if(all.begin(), all.end(),
                                     [&](const services::EmailTemplate& t) { return t.id == req.path_param("id"); });
              if (it == all.end()) throw errors::not_found();
              if (it->owner_id != who.user_id && !who.profile.modify_all_data) throw errors::forbidden();
              all.erase(it);
              services::store_templates(org, all);
              return http::Response::no_content();
          }));

    auto render = [&ctx](http::Request& req, bool send) {
        const engine::Principal& who = principal(req);
        const json::Value body = body_object(req);
        const std::string record_id = body.get_string("recordId");
        engine::Org& org = *who.org;
        services::Rendered out;
        std::string to_user;
        {
            std::shared_lock<std::shared_mutex> lock(org.mutex());
            const std::vector<services::EmailTemplate> all = services::load_templates(org);
            const services::EmailTemplate& t = find_template(all, who, req.path_param("id"));
            const meta::ObjectDef* obj = nullptr;
            std::optional<Record> rec = engine::load_record(org, record_id, &obj);
            engine::AccessChecker access(org, who);
            if (!rec || rec->is_deleted() || !access.can_read(*obj, *rec)) throw errors::not_found("No such record");
            if (obj->name != t.object) throw errors::invalid_field("recordId", "The template is for " + t.object + " records");
            out = services::render_template(org, who, t, *obj, *rec);
            to_user = body.get_string("toUserId", who.user_id);
            if (send && org.users.find(to_user) == nullptr) throw errors::invalid_field("toUserId", "No such user");
        }
        if (send) ctx.records.hooks()->email(org, to_user, out.subject, out.body);
        json::Value v = json::Value::object();
        v.set("subject", out.subject);
        v.set("body", out.body);
        if (send) v.set("sent", true);
        return json_response(200, v);
    };
    r.post("/services/data/v1/emailTemplates/{id}/render",
           authed(ctx, [render](http::Request& req) { return render(req, false); }));
    r.post("/services/data/v1/emailTemplates/{id}/send",
           authed(ctx, [render](http::Request& req) { return render(req, true); }));

    // ---- merge, transfer, activities ---------------------------------------------
    r.post("/services/data/v1/actions/merge", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const json::Value body = body_object(req);
               engine::MergeRequest mr;
               mr.object = body.get_string("object");
               mr.master_id = body.get_string("masterId");
               if (const json::Value* dups = body.find("duplicateIds"); dups && dups->is_array()) {
                   for (const json::Value& d : dups->as_array()) {
                       if (d.is_string()) mr.duplicate_ids.push_back(d.as_string());
                   }
               }
               mr.field_values = body.at("fields").is_object() ? body.at("fields") : json::Value::object();
               const engine::MergeResult res = engine::merge_records(ctx.records, who, mr);
               json::Value v = json::Value::object();
               v.set("id", res.master_id);
               json::Value merged = json::Value::array();
               for (const std::string& id : res.merged_ids) merged.push_back(id);
               v.set("mergedRecordIds", std::move(merged));
               v.set("updatedRelatedIds", res.reparented);
               v.set("success", true);
               return json_response(200, v);
           }));

    r.post("/services/data/v1/actions/transferOwnership", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const json::Value body = body_object(req);
               const engine::TransferResult res = engine::transfer_ownership(
                   ctx.records, who, body.get_string("object"), body.get_string("fromUserId"), body.get_string("toUserId"));
               audit(ctx, who, "Data Management", "Mass transfer of " + body.get_string("object"),
                     std::to_string(res.transferred) + " records to " + body.get_string("toUserId"));
               json::Value v = json::Value::object();
               v.set("transferred", res.transferred);
               json::Value failed = json::Value::array();
               for (const std::string& id : res.failed_ids) failed.push_back(id);
               v.set("failedIds", std::move(failed));
               return json_response(200, v);
           }));

    r.get("/services/data/v1/sobjects/{object}/{id}/activities", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              const Record parent = ctx.records.get(who, req.path_param("object"), req.path_param("id"));
              engine::Org& org = *who.org;
              std::shared_lock<std::shared_mutex> lock(org.mutex());
              engine::AccessChecker access(org, who);
              struct Item {
                  int64_t when;
                  json::Value json;
              };
              std::vector<Item> items;
              for (const char* object : {"Task", "Event"}) {
                  const meta::ObjectDef* obj = org.schema.object(object);
                  if (!access.object_allowed(*obj, engine::ObjectOp::Read)) continue;
                  const storage::Table* t = org.db.find_table(object);
                  if (t == nullptr) continue;
                  for (const auto& [id, rec] : t->rows()) {
                      if (rec.is_deleted()) continue;
                      if (rec.text("WhatId") != parent.id && rec.text("WhoId") != parent.id) continue;
                      if (!access.can_read(*obj, rec)) continue;
                      const FieldValue& when = std::string(object) == "Task" ? rec.get("ActivityDate") : rec.get("StartDateTime");
                      int64_t key = rec.get("CreatedDate").kind() == ValueKind::DateTime ? rec.get("CreatedDate").as_datetime() : 0;
                      if (when.kind() == ValueKind::Date) key = when.as_date() * timeutil::kMsPerDay;
                      if (when.kind() == ValueKind::DateTime) key = when.as_datetime();
                      items.push_back({key, engine::record_to_api_json(*obj, rec, access)});
                  }
              }
              std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.when > b.when; });
              json::Value records = json::Value::array();
              for (Item& i : items) records.push_back(std::move(i.json));
              json::Value v = json::Value::object();
              v.set("totalSize", records.size());
              v.set("records", std::move(records));
              return json_response(200, v);
          }));

    // ---- setup audit trail ---------------------------------------------------------
    r.get("/services/data/v1/setupAuditTrail", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.view_setup || who.profile.modify_all_data, "view the setup audit trail");
              int64_t limit = 100;
              str::parse_int64(req.query_param("limit", "100"), &limit);
              json::Value records = json::Value::array();
              for (const services::AuditEntry& e :
                   ctx.audit.recent(who.org->id(), static_cast<size_t>(std::clamp<int64_t>(limit, 1, 2000)),
                                    req.query_param("section"))) {
                  records.push_back(e.to_json());
              }
              json::Value v = json::Value::object();
              v.set("totalSize", records.size());
              v.set("records", std::move(records));
              return json_response(200, v);
          }));
}

}  // namespace tcrm::api
