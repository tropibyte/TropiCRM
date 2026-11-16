// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/reports.hpp"

#include "model/errors.hpp"
#include "query/ast.hpp"
#include "util/csv.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::services {
namespace {

std::string cell_text(const json::Value& v) {
    switch (v.type()) {
        case json::Value::Type::Null: return "";
        case json::Value::Type::String: return v.as_string();
        case json::Value::Type::Bool: return v.as_bool() ? "true" : "false";
        case json::Value::Type::Number:
            return v.is_integer() ? std::to_string(v.as_int()) : str::format_double(v.as_double(), 6);
        default: return v.dump();
    }
}

/// Reads "Account.Owner.Name" out of a nested query row.
json::Value dig(const json::Value& row, const std::string& path) {
    const json::Value* cur = &row;
    for (const std::string& part : str::split(path, '.')) {
        if (cur == nullptr || !cur->is_object()) return json::Value();
        const json::Value* next = nullptr;
        for (const auto& [k, v] : cur->as_object()) {
            if (str::iequals(k, part)) next = &v;
        }
        cur = next;
    }
    return cur ? *cur : json::Value();
}

}  // namespace

json::Value ReportDef::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("description", description);
    v.set("object", object);
    json::Value cols = json::Value::array();
    for (const std::string& c : columns) cols.push_back(c);
    v.set("columns", std::move(cols));
    json::Value groups = json::Value::array();
    for (const std::string& g : groupings) groups.push_back(g);
    v.set("groupings", std::move(groups));
    v.set("aggregate", aggregate);
    v.set("filter", filter);
    v.set("sortBy", sort_by);
    v.set("sortDescending", sort_descending);
    v.set("rowLimit", row_limit);
    v.set("ownerId", owner_id);
    v.set("public", is_public);
    v.set("createdDate", timeutil::format_datetime(created_at));
    if (last_run_at != 0) v.set("lastRunDate", timeutil::format_datetime(last_run_at));
    return v;
}

ReportDef ReportDef::from_json(const json::Value& v) {
    ReportDef d;
    d.id = v.get_string("id");
    d.name = v.get_string("name");
    d.description = v.get_string("description");
    d.object = v.get_string("object");
    if (const json::Value* cols = v.find("columns"); cols && cols->is_array()) {
        for (const json::Value& c : cols->as_array()) {
            if (c.is_string()) d.columns.push_back(c.as_string());
        }
    }
    if (const json::Value* groups = v.find("groupings"); groups && groups->is_array()) {
        for (const json::Value& g : groups->as_array()) {
            if (g.is_string()) d.groupings.push_back(g.as_string());
        }
    }
    d.aggregate = v.get_string("aggregate", "COUNT()");
    d.filter = v.get_string("filter");
    d.sort_by = v.get_string("sortBy");
    d.sort_descending = v.get_bool("sortDescending");
    d.row_limit = v.get_int("rowLimit", 2000);
    d.owner_id = v.get_string("ownerId");
    d.is_public = v.get_bool("public", true);
    int64_t t = 0;
    if (timeutil::parse_datetime(v.get_string("createdDate"), &t)) d.created_at = t;
    if (timeutil::parse_datetime(v.get_string("lastRunDate"), &t)) d.last_run_at = t;
    return d;
}

std::vector<ReportDef> load_reports(const engine::Org& org) {
    std::vector<ReportDef> out;
    const json::Value* v = org.db.meta("reports");
    if (v == nullptr || !v->is_array()) return out;
    for (const json::Value& r : v->as_array()) out.push_back(ReportDef::from_json(r));
    return out;
}

void store_reports(engine::Org& org, const std::vector<ReportDef>& reports) {
    json::Value arr = json::Value::array();
    for (const ReportDef& r : reports) arr.push_back(r.to_json());
    org.db.put_meta("reports", arr);
}

void validate_report(const engine::Org& org, const ReportDef& def) {
    if (str::trim(def.name).empty()) throw errors::invalid_field("name", "Report name is required");
    if (org.schema.object(def.object) == nullptr) throw errors::invalid_field("object", "Unknown object " + def.object);
    if (!def.is_summary() && def.columns.empty()) {
        throw errors::invalid_field("columns", "A tabular report needs at least one column");
    }
    if (def.groupings.size() > 3) throw errors::invalid_field("groupings", "At most three groupings are allowed");
    if (def.row_limit < 1 || def.row_limit > 50000) throw errors::invalid_field("rowLimit", "rowLimit must be 1-50000");
    // Parse once now so a broken report is rejected when it's saved.
    query::parse(report_query(def));
}

std::string report_query(const ReportDef& def) {
    std::string q = "SELECT ";
    if (def.is_summary()) {
        q += str::join(def.groupings, ", ") + ", " + (def.aggregate.empty() ? "COUNT()" : def.aggregate) + " value";
    } else {
        q += str::join(def.columns, ", ");
    }
    q += " FROM " + def.object;
    if (!str::trim(def.filter).empty()) q += " WHERE " + def.filter;
    if (def.is_summary()) q += " GROUP BY " + str::join(def.groupings, ", ");
    if (!def.sort_by.empty()) q += " ORDER BY " + def.sort_by + (def.sort_descending ? " DESC" : " ASC");
    q += " LIMIT " + std::to_string(def.row_limit);
    return q;
}

json::Value run_report(const engine::Principal& who, const ReportDef& def) {
    query::ExecOptions opts;
    opts.batch_size = def.row_limit;
    const query::QueryResult result = query::execute(report_query(def), who, opts, nullptr);

    json::Value meta = json::Value::object();
    meta.set("id", def.id);
    meta.set("name", def.name);
    meta.set("reportFormat", def.is_summary() ? "SUMMARY" : "TABULAR");
    meta.set("query", report_query(def));

    json::Value out = json::Value::object();
    out.set("reportMetadata", std::move(meta));
    json::Value rows = json::Value::array();
    double grand_total = 0;
    for (const json::Value& rec : result.records.as_array()) {
        json::Value row = json::Value::array();
        if (def.is_summary()) {
            for (const std::string& g : def.groupings) row.push_back(dig(rec, str::split(g, '.').back()));
            const json::Value& v = rec.at("value");
            row.push_back(v);
            if (v.is_number()) grand_total += v.as_double();
        } else {
            for (const std::string& c : def.columns) row.push_back(dig(rec, c));
        }
        rows.push_back(std::move(row));
    }
    out.set("rows", std::move(rows));
    out.set("totalSize", result.total_size);
    if (def.is_summary()) out.set("grandTotal", grand_total);
    return out;
}

std::string report_to_csv(const ReportDef& def, const json::Value& run) {
    std::string out;
    std::vector<std::string> header = def.is_summary() ? def.groupings : def.columns;
    if (def.is_summary()) header.push_back(def.aggregate.empty() ? "COUNT()" : def.aggregate);
    out += csv::format_row(header);
    const json::Value& rows = run.at("rows");
    if (!rows.is_array()) return out;
    for (const json::Value& row : rows.as_array()) {
        std::vector<std::string> cells;
        for (const json::Value& cell : row.as_array()) cells.push_back(cell_text(cell));
        out += csv::format_row(cells);
    }
    return out;
}

}  // namespace tcrm::services
