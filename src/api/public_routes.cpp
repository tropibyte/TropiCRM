// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <mutex>
#include <shared_mutex>
#include <sstream>

#include "api/context.hpp"
#include "engine/records.hpp"
#include "util/strutil.hpp"
#include "util/url.hpp"

namespace tcrm::api {
namespace {

/// Web forms post either form-encoded or JSON bodies.
json::Value form_fields(const http::Request& req) {
    const std::string ctype = str::to_lower(req.headers.get("Content-Type"));
    if (ctype.find("application/json") != std::string::npos) return body_object(req);
    json::Value out = json::Value::object();
    for (const auto& [k, v] : url::parse_query(req.body)) out.set(k, v);
    return out;
}

/// The user web-form records are created as: the org's first active
/// administrator.
std::string default_owner(const engine::Org& org) {
    for (const auth::User& u : org.users.all()) {
        const security::Profile* p = org.profiles.find(u.profile_id);
        if (u.active && p != nullptr && p->modify_all_data) return u.id;
    }
    return {};
}

http::Response web_form(ApiContext& ctx, http::Request& req, const std::string& object) {
    json::Value fields = form_fields(req);
    std::string org_id = req.query_param("oid");
    if (org_id.empty()) org_id = fields.get_string("oid");
    fields.erase("oid");
    const std::string ret_url = fields.get_string("retURL");
    fields.erase("retURL");
    auto org = ctx.registry.find(org_id);
    if (org == nullptr || !org->active()) throw errors::not_found("Unknown organization");
    Record created;
    {
        std::unique_lock<std::shared_mutex> lock(org->mutex());
        if (!org->settings.web_forms) throw ApiError(403, "WEB_FORMS_DISABLED", "Web forms are disabled for this org");
        const meta::ObjectDef* obj = org->schema.object(object);
        const engine::Principal system = engine::Principal::system_for(org, default_owner(*org));
        if (object == "Case") {
            if (fields.contains("email")) {
                fields.set("SuppliedEmail", fields.get_string("email"));
                fields.erase("email");
            }
            if (fields.contains("name")) {
                fields.set("SuppliedName", fields.get_string("name"));
                fields.erase("name");
            }
            if (!fields.contains("Origin")) fields.set("Origin", "Web");
        } else if (!fields.contains("LeadSource")) {
            fields.set("LeadSource", "Web");
        }
        engine::SaveOptions opts;
        opts.allow_duplicates = true;
        created = ctx.records.create_locked(*org, system, *obj, fields, opts);
    }
    if (!ret_url.empty()) {
        http::Response redirect;
        redirect.status = 303;
        redirect.headers.set("Location", ret_url);
        return redirect;
    }
    json::Value v = json::Value::object();
    v.set("success", true);
    v.set("id", created.id);
    return json_response(201, v);
}

std::string label_value(const std::string& s) {
    return str::replace_all(str::replace_all(s, "\\", "\\\\"), "\n", "\\n");
}

}  // namespace

void register_public_routes(http::Router& r, ApiContext& ctx) {
    r.post("/services/web2lead", [&ctx](http::Request& req) { return web_form(ctx, req, "Lead"); });
    r.post("/services/web2case", [&ctx](http::Request& req) { return web_form(ctx, req, "Case"); });

    // Prometheus exposition for the operations dashboard.
    r.get("/metrics", [&ctx](http::Request&) {
        std::ostringstream out;
        out << "# HELP tropicrm_http_responses_total HTTP responses by status class.\n"
            << "# TYPE tropicrm_http_responses_total counter\n"
            << "tropicrm_http_responses_total{class=\"2xx\"} " << ctx.metrics.responses_2xx.load() << "\n"
            << "tropicrm_http_responses_total{class=\"3xx\"} " << ctx.metrics.responses_3xx.load() << "\n"
            << "tropicrm_http_responses_total{class=\"4xx\"} " << ctx.metrics.responses_4xx.load() << "\n"
            << "tropicrm_http_responses_total{class=\"5xx\"} " << ctx.metrics.responses_5xx.load() << "\n"
            << "# HELP tropicrm_http_latency_ms_total Sum of request latencies.\n"
            << "# TYPE tropicrm_http_latency_ms_total counter\n"
            << "tropicrm_http_latency_ms_total " << ctx.metrics.total_latency_ms.load() << "\n"
            << "# HELP tropicrm_query_cursors_open Open query cursors.\n"
            << "# TYPE tropicrm_query_cursors_open gauge\n"
            << "tropicrm_query_cursors_open " << ctx.cursors.open_cursors() << "\n"
            << "# HELP tropicrm_webhooks_pending Webhook deliveries waiting to be sent.\n"
            << "# TYPE tropicrm_webhooks_pending gauge\n"
            << "tropicrm_webhooks_pending " << ctx.webhooks.pending() << "\n"
            << "# HELP tropicrm_org_api_calls_today API calls per org today.\n"
            << "# TYPE tropicrm_org_api_calls_today gauge\n";
        std::ostringstream users, records;
        for (const auto& org : ctx.registry.all()) {
            const std::string labels =
                "{org_id=\"" + org->id() + "\",org_name=\"" + label_value(org->name()) + "\"}";
            out << "tropicrm_org_api_calls_today" << labels << " " << org->usage.calls.load() << "\n";
            std::shared_lock<std::shared_mutex> lock(org->mutex());
            users << "tropicrm_org_users" << labels << " " << org->users.size() << "\n";
            size_t n = 0;
            for (const std::string& t : org->db.table_names()) n += org->db.find_table(t)->size();
            records << "tropicrm_org_records" << labels << " " << n << "\n";
        }
        out << "# HELP tropicrm_org_users Users per org.\n# TYPE tropicrm_org_users gauge\n" << users.str();
        out << "# HELP tropicrm_org_records Stored records per org.\n# TYPE tropicrm_org_records gauge\n" << records.str();
        return http::Response::text(200, out.str(), "text/plain; version=0.0.4");
    });
}

}  // namespace tcrm::api
