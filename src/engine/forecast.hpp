// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "engine/access.hpp"
#include "json/json.hpp"

namespace tcrm::engine {

/// Opportunity amounts rolled up by close month and forecast category for
/// a user and the people who report up to them through the role hierarchy.
struct ForecastRow {
    std::string period;  // "2026-11"
    double pipeline = 0;
    double best_case = 0;
    double commit = 0;
    double closed = 0;
    int64_t opportunities = 0;
};

struct Forecast {
    std::string user_id;
    std::string currency;
    std::vector<std::string> owner_ids;  // whose opportunities were included
    std::vector<ForecastRow> rows;       // sorted by period
    ForecastRow total;

    json::Value to_json() const;
};

/// Builds the forecast for `who` covering close dates in [start_day, end_day).
/// Takes the org's shared lock.
Forecast build_forecast(const Principal& who, int64_t start_day, int64_t end_day);

/// The first day of the current quarter and of the one after it, in the
/// org's time zone.
std::pair<int64_t, int64_t> current_quarter(const Org& org);

}  // namespace tcrm::engine
