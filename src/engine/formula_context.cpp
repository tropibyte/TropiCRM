// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/formula_context.hpp"

#include "engine/records.hpp"
#include "model/errors.hpp"
#include "util/strutil.hpp"

namespace tcrm::engine {

RecordFormulaContext::RecordFormulaContext(const Org& org, const meta::ObjectDef& obj, const Record& record,
                                           const Record* prior, const Principal& who)
    : org_(org), obj_(obj), record_(record), prior_(prior), who_(who), tz_(org.tz()) {}

FieldValue RecordFormulaContext::field(const std::string& path) {
    const std::vector<std::string> parts = str::split(path, '.');
    const meta::ObjectDef* cur_obj = &obj_;
    const Record* cur = &record_;
    std::optional<Record> holder;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const meta::FieldDef* link = cur_obj->relationship_field(parts[i]);
        if (link == nullptr) throw formula::FormulaError("unknown relationship " + parts[i], 0);
        const FieldValue& id = cur->get(link->name);
        if (id.kind() != ValueKind::String) return FieldValue::null();
        const meta::ObjectDef* next_obj = nullptr;
        std::optional<Record> next = load_record(org_, id.as_string(), &next_obj);
        if (!next) return FieldValue::null();
        holder = std::move(next);
        cur = &*holder;
        cur_obj = next_obj;
    }
    const meta::FieldDef* f = cur_obj->field(parts.back());
    if (f == nullptr) throw formula::FormulaError("unknown field " + parts.back() + " on " + cur_obj->name, 0);
    return cur->get(f->name);
}

FieldValue RecordFormulaContext::prior(const std::string& field) {
    if (prior_ == nullptr) return FieldValue::null();
    const meta::FieldDef* f = obj_.field(field);
    if (f == nullptr) throw formula::FormulaError("unknown field " + field, 0);
    return prior_->get(f->name);
}

FieldValue RecordFormulaContext::global(const std::string& name) {
    if (str::iequals(name, "$User.Id")) return FieldValue::string(who_.user_id);
    if (str::iequals(name, "$User.Username")) return FieldValue::string(who_.username);
    if (str::iequals(name, "$User.RoleId")) return FieldValue::string(who_.role_id);
    if (str::iequals(name, "$Profile.Name")) return FieldValue::string(who_.profile.name);
    if (str::iequals(name, "$Organization.Id")) return FieldValue::string(org_.id());
    if (str::iequals(name, "$Organization.Name")) return FieldValue::string(org_.name());
    throw formula::FormulaError("unknown global " + name, 0);
}

FormulaCache& FormulaCache::instance() {
    static FormulaCache cache;
    return cache;
}

std::shared_ptr<const formula::Formula> FormulaCache::get(const std::string& source) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(source);
        if (it != cache_.end()) return it->second;
    }
    auto compiled = std::make_shared<const formula::Formula>(formula::Formula::compile(source));
    std::lock_guard<std::mutex> lock(mu_);
    cache_.emplace(source, compiled);
    return compiled;
}

FieldValue evaluate_formula(const std::string& source, const Org& org, const meta::ObjectDef& obj, const Record& r,
                            const Record* prior, const Principal& who) {
    try {
        auto f = FormulaCache::instance().get(source);
        RecordFormulaContext ctx(org, obj, r, prior, who);
        return f->evaluate(ctx);
    } catch (const formula::FormulaError& e) {
        throw errors::bad_request("FORMULA_EVALUATION_ERROR", std::string("Error evaluating formula: ") + e.what());
    }
}

}  // namespace tcrm::engine
