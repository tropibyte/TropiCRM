// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <vector>

#include "engine/access.hpp"
#include "json/json.hpp"

namespace tcrm::services {

struct DashboardComponent {
    std::string title;
    std::string report_id;
    std::string display = "table";  // table, bar, metric, donut
};

/// A dashboard shows several reports at once. Reports run as the
/// dashboard's running user, so everyone who opens it sees the same data.
struct Dashboard {
    std::string id;
    std::string name;
    std::string description;
    std::string owner_id;
    std::string running_user_id;  // empty = the viewer
    std::vector<DashboardComponent> components;
    int64_t created_at = 0;

    json::Value to_json() const;
    static Dashboard from_json(const json::Value& v);
};

std::vector<Dashboard> load_dashboards(const engine::Org& org);
void store_dashboards(engine::Org& org, const std::vector<Dashboard>& dashboards);

/// Validates a dashboard definition. Throws ApiError.
void validate_dashboard(const engine::Org& org, Dashboard* d);

/// Runs every component's report and returns
/// {"dashboard": ..., "components": [{"title", "display", "result"|"error"}]}.
json::Value run_dashboard(const engine::Principal& viewer, const Dashboard& d);

}  // namespace tcrm::services
