// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <vector>

#include "engine/access.hpp"
#include "json/json.hpp"
#include "query/executor.hpp"

namespace tcrm::services {

/// A saved report: a tabular list of columns, or a summary grouped by up
/// to three fields with one aggregate.
struct ReportDef {
    std::string id;
    std::string name;
    std::string description;
    std::string object;
    std::vector<std::string> columns;    // tabular reports
    std::vector<std::string> groupings;  // summary reports
    std::string aggregate = "COUNT()";   // summary reports, e.g. "SUM(Amount)"
    std::string filter;                  // a TQL WHERE clause
    std::string sort_by;
    bool sort_descending = false;
    int64_t row_limit = 2000;
    std::string owner_id;
    bool is_public = true;
    int64_t created_at = 0;
    int64_t last_run_at = 0;

    bool is_summary() const { return !groupings.empty(); }

    json::Value to_json() const;
    static ReportDef from_json(const json::Value& v);
};

std::vector<ReportDef> load_reports(const engine::Org& org);
void store_reports(engine::Org& org, const std::vector<ReportDef>& reports);

/// Throws ApiError for definitions that can't run.
void validate_report(const engine::Org& org, const ReportDef& def);

/// The TQL a report runs.
std::string report_query(const ReportDef& def);

/// Runs a report as `who`: {"reportMetadata": ..., "factMap": {...}, "rows": [...]}.
json::Value run_report(const engine::Principal& who, const ReportDef& def);

/// Flattens a report run into CSV with a header row.
std::string report_to_csv(const ReportDef& def, const json::Value& run);

}  // namespace tcrm::services
