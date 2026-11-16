// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/forecast.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <shared_mutex>

#include "engine/record_service.hpp"
#include "util/time.hpp"

namespace tcrm::engine {
namespace {

std::string month_key(int64_t day) {
    const timeutil::Civil c = timeutil::civil_from_days(day);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d", c.year, c.month);
    return buf;
}

void add(ForecastRow* row, const std::string& category, double amount) {
    ++row->opportunities;
    if (category == "Closed") row->closed += amount;
    else if (category == "Commit") row->commit += amount;
    else if (category == "Best Case") row->best_case += amount;
    else if (category == "Pipeline") row->pipeline += amount;
}

json::Value row_json(const ForecastRow& r) {
    json::Value v = json::Value::object();
    if (!r.period.empty()) v.set("period", r.period);
    v.set("pipeline", r.pipeline);
    v.set("bestCase", r.best_case);
    v.set("commit", r.commit);
    v.set("closed", r.closed);
    // Forecast categories are cumulative, as on the forecasts page.
    v.set("bestCaseTotal", r.best_case + r.commit + r.closed);
    v.set("commitTotal", r.commit + r.closed);
    v.set("opportunities", r.opportunities);
    return v;
}

}  // namespace

json::Value Forecast::to_json() const {
    json::Value v = json::Value::object();
    v.set("userId", user_id);
    v.set("currency", currency);
    json::Value owners = json::Value::array();
    for (const std::string& o : owner_ids) owners.push_back(o);
    v.set("ownerIds", std::move(owners));
    json::Value rs = json::Value::array();
    for (const ForecastRow& r : rows) rs.push_back(row_json(r));
    v.set("periods", std::move(rs));
    v.set("total", row_json(total));
    return v;
}

std::pair<int64_t, int64_t> current_quarter(const Org& org) {
    const int64_t today = org.tz().local_date(timeutil::now_ms());
    const timeutil::Civil c = timeutil::civil_from_days(today);
    const int first_month = ((c.month - 1) / 3) * 3 + 1;
    const int64_t start = timeutil::days_from_civil(c.year, first_month, 1);
    return {start, timeutil::add_months(start, 3)};
}

Forecast build_forecast(const Principal& who, int64_t start_day, int64_t end_day) {
    const Org& org = *who.org;
    std::shared_lock<std::shared_mutex> lock(org.mutex());
    Forecast f;
    f.user_id = who.user_id;
    f.currency = org.settings.currency;

    // The forecast covers the user plus everyone in their part of the role
    // hierarchy.
    std::set<std::string> owners = {who.user_id};
    if (!who.role_id.empty()) {
        const std::vector<std::string> roles = org.roles.role_and_subordinates(who.role_id);
        for (const auth::User& u : org.users.all()) {
            if (std::find(roles.begin(), roles.end(), u.role_id) != roles.end()) owners.insert(u.id);
        }
    }
    f.owner_ids.assign(owners.begin(), owners.end());

    std::map<std::string, ForecastRow> by_month;
    const storage::Table* opps = org.db.find_table("Opportunity");
    if (opps != nullptr) {
        for (const auto& [id, r] : opps->rows()) {
            if (r.is_deleted() || owners.count(r.owner_id()) == 0) continue;
            const FieldValue& close = r.get("CloseDate");
            if (close.kind() != ValueKind::Date) continue;
            const int64_t day = close.as_date();
            if (day < start_day || day >= end_day) continue;
            const FieldValue& amount_v = r.get("Amount");
            double amount = amount_v.is_numeric() ? amount_v.as_double() : 0.0;
            const std::string code = r.text("CurrencyIsoCode");
            if (!code.empty() && code != f.currency) amount = convert_currency(org.settings, amount, code, f.currency);
            const std::string category = r.text("ForecastCategoryName");
            ForecastRow& row = by_month[month_key(day)];
            row.period = month_key(day);
            add(&row, category, amount);
            add(&f.total, category, amount);
        }
    }
    for (auto& [k, row] : by_month) f.rows.push_back(row);
    return f;
}

}  // namespace tcrm::engine
