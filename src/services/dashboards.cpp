// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/dashboards.hpp"

#include <shared_mutex>

#include "model/errors.hpp"
#include "services/reports.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::services {
namespace {

bool valid_display(const std::string& d) {
    return d == "table" || d == "bar" || d == "metric" || d == "donut";
}

/// The principal a dashboard's reports run as.
engine::Principal running_principal(const engine::Principal& viewer, const Dashboard& d) {
    if (d.running_user_id.empty() || d.running_user_id == viewer.user_id) return viewer;
    std::shared_lock<std::shared_mutex> lock(viewer.org->mutex());
    const auth::User* u = viewer.org->users.find(d.running_user_id);
    if (u == nullptr || !u->active) throw errors::bad_request("INVALID_RUNNING_USER", "The running user is inactive");
    engine::Principal p;
    p.org = viewer.org;
    p.user_id = u->id;
    p.username = u->username;
    p.role_id = u->role_id;
    p.timezone = u->timezone;
    if (const security::Profile* prof = viewer.org->profiles.find(u->profile_id)) p.profile = *prof;
    return p;
}

}  // namespace

json::Value Dashboard::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("description", description);
    v.set("ownerId", owner_id);
    v.set("runningUserId", running_user_id);
    json::Value comps = json::Value::array();
    for (const DashboardComponent& c : components) {
        json::Value jc = json::Value::object();
        jc.set("title", c.title);
        jc.set("reportId", c.report_id);
        jc.set("display", c.display);
        comps.push_back(std::move(jc));
    }
    v.set("components", std::move(comps));
    v.set("createdDate", timeutil::format_datetime(created_at));
    return v;
}

Dashboard Dashboard::from_json(const json::Value& v) {
    Dashboard d;
    d.id = v.get_string("id");
    d.name = v.get_string("name");
    d.description = v.get_string("description");
    d.owner_id = v.get_string("ownerId");
    d.running_user_id = v.get_string("runningUserId");
    if (const json::Value* comps = v.find("components"); comps && comps->is_array()) {
        for (const json::Value& jc : comps->as_array()) {
            DashboardComponent c;
            c.title = jc.get_string("title");
            c.report_id = jc.get_string("reportId");
            c.display = jc.get_string("display", "table");
            d.components.push_back(std::move(c));
        }
    }
    timeutil::parse_datetime(v.get_string("createdDate"), &d.created_at);
    return d;
}

std::vector<Dashboard> load_dashboards(const engine::Org& org) {
    std::vector<Dashboard> out;
    if (const json::Value* v = org.db.meta("dashboards"); v && v->is_array()) {
        for (const json::Value& d : v->as_array()) out.push_back(Dashboard::from_json(d));
    }
    return out;
}

void store_dashboards(engine::Org& org, const std::vector<Dashboard>& dashboards) {
    json::Value arr = json::Value::array();
    for (const Dashboard& d : dashboards) arr.push_back(d.to_json());
    org.db.put_meta("dashboards", arr);
}

void validate_dashboard(const engine::Org& org, Dashboard* d) {
    if (str::trim(d->name).empty()) throw errors::invalid_field("name", "Dashboard name is required");
    if (d->components.empty() || d->components.size() > 20) {
        throw errors::invalid_field("components", "A dashboard has 1 to 20 components");
    }
    const std::vector<ReportDef> reports = load_reports(org);
    for (DashboardComponent& c : d->components) {
        if (!valid_display(c.display)) throw errors::invalid_field("display", "Unknown display type " + c.display);
        bool found = false;
        for (const ReportDef& r : reports) found = found || r.id == c.report_id;
        if (!found) throw errors::invalid_field("reportId", "No such report " + c.report_id);
        if (c.title.empty()) c.title = c.report_id;
    }
    if (!d->running_user_id.empty()) {
        const auth::User* u = org.users.find(d->running_user_id);
        if (u == nullptr || !u->active) throw errors::invalid_field("runningUserId", "No such active user");
    }
}

json::Value run_dashboard(const engine::Principal& viewer, const Dashboard& d) {
    const engine::Principal runner = running_principal(viewer, d);
    std::vector<ReportDef> reports;
    {
        std::shared_lock<std::shared_mutex> lock(viewer.org->mutex());
        reports = load_reports(*viewer.org);
    }
    json::Value comps = json::Value::array();
    for (const DashboardComponent& c : d.components) {
        json::Value jc = json::Value::object();
        jc.set("title", c.title);
        jc.set("display", c.display);
        const ReportDef* def = nullptr;
        for (const ReportDef& r : reports) {
            if (r.id == c.report_id) def = &r;
        }
        if (def == nullptr) {
            jc.set("error", "The report was deleted");
        } else {
            try {
                jc.set("result", run_report(runner, *def));
            } catch (const ApiError& e) {
                jc.set("error", std::string(e.what()));
            }
        }
        comps.push_back(std::move(jc));
    }
    json::Value out = json::Value::object();
    out.set("dashboard", d.to_json());
    out.set("runningUserId", runner.user_id);
    out.set("components", std::move(comps));
    return out;
}

}  // namespace tcrm::services
