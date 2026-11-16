// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <shared_mutex>

#include "api/context.hpp"
#include "engine/records.hpp"
#include "meta/codec.hpp"
#include "util/id.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::api {

const engine::Principal& principal(const http::Request& req) {
    const auto* p = std::any_cast<engine::Principal>(&req.auth);
    if (p == nullptr) throw errors::unauthorized();
    return *p;
}

http::Response json_response(int status, const json::Value& body) {
    return http::Response::json(status, body);
}

http::Response error_response(const ApiError& e) {
    json::Value err = json::Value::object();
    err.set("errorCode", e.code());
    err.set("message", e.what());
    if (!e.fields().empty()) {
        json::Value fields = json::Value::array();
        for (const std::string& f : e.fields()) fields.push_back(f);
        err.set("fields", std::move(fields));
    }
    json::Value body = json::Value::array();
    body.push_back(std::move(err));
    // OAuth endpoints use their own error shape.
    if (e.code() == "invalid_grant" || e.code() == "invalid_request" || e.code() == "unsupported_grant_type") {
        body = json::Value::object();
        body.set("error", e.code());
        body.set("error_description", e.what());
    }
    return http::Response::json(e.status(), body);
}

json::Value body_object(const http::Request& req) {
    json::Value v;
    std::string err;
    if (!json::try_parse(str::trim(req.body).empty() ? std::string("{}") : req.body, &v, &err)) {
        throw errors::bad_request("JSON_PARSER_ERROR", "Malformed JSON: " + err);
    }
    if (!v.is_object()) throw errors::bad_request("JSON_PARSER_ERROR", "Expected a JSON object");
    return v;
}

void require(bool allowed, const std::string& what) {
    if (!allowed) throw ApiError(403, "INSUFFICIENT_ACCESS", "You don't have permission to " + what);
}

void audit(ApiContext& ctx, const engine::Principal& who, const std::string& section, const std::string& action,
           const std::string& detail) {
    ctx.audit.record(who.org->id(), who.user_id, who.username, section, action, detail);
}

std::string new_config_id(engine::Org& org, const std::string& prefix) {
    return org.new_id(prefix);
}

http::Handler authed(ApiContext& ctx, http::Handler handler) {
    return [&ctx, handler = std::move(handler)](http::Request& req) -> http::Response {
        engine::Principal who = ctx.auth.authenticate(req);
        engine::Org& org = *who.org;
        if (!org.active()) throw errors::unauthorized("This organization has been disabled");
        if (!who.profile.api_enabled) throw ApiError(403, "API_DISABLED_FOR_ORG", "API is not enabled for this user");

        // Daily API limit.
        const int64_t today = timeutil::today_utc();
        if (org.usage.day.load() != today) {
            org.usage.day = today;
            org.usage.calls = 0;
        }
        const uint32_t limit = static_cast<uint32_t>(org.settings.api_calls_per_day);
        const uint32_t used = org.usage.calls.fetch_add(1) + 1;
        const uint32_t remaining = limit - used;
        if (remaining == 0) throw errors::limit_exceeded("TotalRequests Limit exceeded.");
        req.auth = std::move(who);
        http::Response resp = handler(req);
        resp.headers.set("Sforce-Limit-Info", "api-usage=" + std::to_string(used) + "/" + std::to_string(limit));
        return resp;
    };
}

json::Value describe_object(const engine::Org& org, const engine::AccessChecker& access, const meta::ObjectDef& obj) {
    json::Value d = json::Value::object();
    d.set("name", obj.name);
    d.set("label", obj.label);
    d.set("labelPlural", obj.plural_label);
    d.set("keyPrefix", obj.key_prefix);
    d.set("custom", obj.custom);
    d.set("searchable", obj.searchable);
    d.set("queryable", obj.queryable);
    d.set("createable", access.object_allowed(obj, engine::ObjectOp::Create) && obj.name != "User");
    d.set("updateable", access.object_allowed(obj, engine::ObjectOp::Edit) && obj.name != "User");
    d.set("deletable", access.object_allowed(obj, engine::ObjectOp::Delete) && obj.name != "User");
    d.set("description", obj.description);
    json::Value fields = json::Value::array();
    for (const meta::FieldDef& f : obj.fields) {
        if (!access.field_readable(obj, f)) continue;
        json::Value jf = meta::Schema::field_to_json(f);
        jf.set("createable", access.field_editable(obj, f));
        jf.set("updateable", access.field_editable(obj, f) && f.is_updateable());
        jf.set("calculated", f.is_calculated());
        jf.set("nillable", !f.required);
        jf.set("custom", f.custom);
        jf.set("nameField", f.name_field);
        fields.push_back(std::move(jf));
    }
    d.set("fields", std::move(fields));
    json::Value children = json::Value::array();
    for (const meta::ChildRelationship& c : obj.children) {
        json::Value jc = json::Value::object();
        jc.set("childSObject", c.child_object);
        jc.set("field", c.field);
        jc.set("relationshipName", c.relationship_name);
        jc.set("cascadeDelete", c.delete_rule == meta::DeleteRule::Cascade);
        jc.set("restrictedDelete", c.delete_rule == meta::DeleteRule::Restrict);
        children.push_back(std::move(jc));
    }
    d.set("childRelationships", std::move(children));
    json::Value urls = json::Value::object();
    const std::string base = std::string(engine::kApiBase) + "/sobjects/" + obj.name;
    urls.set("sobject", base);
    urls.set("describe", base + "/describe");
    urls.set("rowTemplate", base + "/{ID}");
    d.set("urls", std::move(urls));
    (void)org;
    return d;
}

}  // namespace tcrm::api
