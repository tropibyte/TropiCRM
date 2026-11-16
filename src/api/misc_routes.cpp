// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "api/context.hpp"
#include "engine/lead_convert.hpp"
#include "util/time.hpp"

namespace tcrm::api {

void register_misc_routes(http::Router& r, ApiContext& ctx) {
    r.get("/healthz", [&ctx](http::Request&) {
        json::Value v = json::Value::object();
        v.set("status", "ok");
        v.set("uptimeSeconds", (timeutil::now_ms() - ctx.started_at) / 1000);
        return json_response(200, v);
    });

    r.post("/services/data/v1/actions/convertLead", authed(ctx, [&ctx](http::Request& req) {
               const engine::Principal& who = principal(req);
               const json::Value body = body_object(req);
               engine::LeadConvertRequest cr;
               cr.lead_id = body.get_string("leadId");
               cr.account_id = body.get_string("accountId");
               cr.contact_id = body.get_string("contactId");
               cr.create_opportunity = !body.get_bool("doNotCreateOpportunity");
               cr.opportunity_name = body.get_string("opportunityName");
               cr.converted_status = body.get_string("convertedStatus", "Closed - Converted");
               cr.owner_id = body.get_string("ownerId");
               if (cr.lead_id.empty()) throw errors::invalid_field("leadId", "leadId is required");
               const engine::LeadConvertResult res = engine::convert_lead(ctx.records, who, cr);
               json::Value v = json::Value::object();
               v.set("success", true);
               v.set("leadId", res.lead_id);
               v.set("accountId", res.account_id);
               v.set("contactId", res.contact_id);
               v.set("opportunityId", res.opportunity_id.empty() ? json::Value() : json::Value(res.opportunity_id));
               v.set("accountCreated", res.account_created);
               return json_response(200, v);
           }));

    r.get("/services/data/v1/outbox", authed(ctx, [&ctx](http::Request& req) {
              const engine::Principal& who = principal(req);
              require(who.profile.view_setup || who.profile.modify_all_data, "view the email outbox");
              json::Value records = json::Value::array();
              for (const services::OutgoingEmail& m : ctx.outbox.recent(who.org->id(), 100)) records.push_back(m.to_json());
              json::Value v = json::Value::object();
              v.set("records", std::move(records));
              return json_response(200, v);
          }));
}

}  // namespace tcrm::api
