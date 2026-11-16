// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/record_service.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>

#include "engine/approvals.hpp"
#include "engine/formula_context.hpp"
#include "engine/records.hpp"
#include "meta/codec.hpp"
#include "model/errors.hpp"
#include "util/id.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::engine {

using meta::FieldDef;
using meta::FieldType;
using meta::ObjectDef;

namespace {

constexpr int kMaxWorkflowDepth = 5;
constexpr const char* kHistoryTable = "_History";
constexpr const char* kDeleteBatch = "_DeleteBatch";

struct WorkflowDepthGuard {
    explicit WorkflowDepthGuard(int& depth) : depth_(depth) { ++depth_; }
    ~WorkflowDepthGuard() { --depth_; }
    WorkflowDepthGuard(const WorkflowDepthGuard&) = delete;
    WorkflowDepthGuard& operator=(const WorkflowDepthGuard&) = delete;
    int& depth_;
};

thread_local int t_workflow_depth = 0;

std::string format_autonumber(const std::string& format, int64_t n) {
    const size_t open = format.find('{');
    const size_t close = format.find('}', open);
    if (open == std::string::npos || close == std::string::npos) return format + std::to_string(n);
    const int width = static_cast<int>(close - open - 1);
    return format.substr(0, open) + str::zero_pad(n, width) + format.substr(close + 1);
}

std::string normalize_company(const std::string& name) {
    std::string lowered = str::to_lower(name);
    std::string out;
    for (char c : lowered) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || static_cast<unsigned char>(c) >= 0x80) out += c;
        else if (c == ' ' && !out.empty() && out.back() != ' ') out += ' ';
    }
    static const char* const kSuffixes[] = {" incorporated", " inc", " llc", " ltd", " limited", " corp",
                                            " corporation", " co", " gmbh", " plc"};
    for (const char* suffix : kSuffixes) {
        if (str::iends_with(out, suffix)) {
            out.resize(out.size() - std::char_traits<char>::length(suffix));
            break;
        }
    }
    return std::string(str::trim(out));
}

json::Value history_value(const FieldDef& f, const FieldValue& v) {
    return meta::value_to_json(f, v);
}

FieldValue coerce_formula_result(const FieldDef& f, const FieldValue& v) {
    if (v.is_null()) return v;
    switch (f.formula_return) {
        case FieldType::Number:
        case FieldType::Currency:
        case FieldType::Percent:
            if (!v.is_numeric()) return FieldValue::null();
            return FieldValue::number(std::round(v.as_double() * std::pow(10.0, f.scale)) / std::pow(10.0, f.scale));
        case FieldType::Checkbox: return v.kind() == ValueKind::Bool ? v : FieldValue::null();
        case FieldType::Date: return v.kind() == ValueKind::Date ? v : FieldValue::null();
        case FieldType::DateTime: return v.kind() == ValueKind::DateTime ? v : FieldValue::null();
        default: {
            std::string s = v.to_text();
            if (s.empty()) return FieldValue::null();
            if (static_cast<int>(s.size()) > 3900) s = str::ellipsize(s, 3900);
            return FieldValue::string(std::move(s));
        }
    }
}

bool reference_allowed(const FieldDef& f, const std::string& object) {
    if (f.reference_to == "*") return true;
    for (const std::string& t : str::split_trimmed(f.reference_to, ',')) {
        if (str::iequals(t, object)) return true;
    }
    return false;
}

}  // namespace

const ObjectDef& RecordService::require_object(const Org& org, std::string_view name) {
    const ObjectDef* obj = org.schema.object(name);
    if (obj == nullptr) throw errors::not_found("The requested resource does not exist: sObject type '" + std::string(name) + "'");
    return *obj;
}

// ---- field handling ---------------------------------------------------------

void RecordService::apply_fields(const Org& org, const AccessChecker& access, const ObjectDef& obj,
                                 const json::Value& fields, Mode mode, Record* draft) const {
    (void)org;
    if (!fields.is_object()) throw errors::bad_request("JSON_PARSER_ERROR", "The request body must be a JSON object");
    std::vector<std::string> not_writable;
    for (const auto& [key, value] : fields.as_object()) {
        if (key == "attributes") continue;
        const FieldDef* f = obj.field(key);
        if (f == nullptr) {
            throw errors::bad_request("INVALID_FIELD", "No such column '" + key + "' on sobject of type " + obj.name,
                                      {key});
        }
        if (f->name == "Id") {
            if (mode != Mode::Create && value.is_string() && value.as_string() == draft->id) continue;
            not_writable.push_back(f->name);
            continue;
        }
        if (mode == Mode::Upsert) {
            if (!f->is_createable() && f->name != "OwnerId") {
                not_writable.push_back(f->name);
                continue;
            }
        } else if (!access.field_editable(obj, *f)) {
            not_writable.push_back(f->name);
            continue;
        }
        draft->set(f->name, meta::value_from_json(*f, value));
    }
    if (!not_writable.empty()) {
        throw errors::bad_request("INVALID_FIELD_FOR_INSERT_UPDATE",
                                  "Unable to create/update fields: " + str::join(not_writable, ", ") +
                                      ". Please check the security settings of this field and verify that it is "
                                      "read/write for your profile or permission set.",
                                  not_writable);
    }
}

void RecordService::check_references(const Org& org, const ObjectDef& obj, const Record& r, const Record* before) const {
    for (const FieldDef& f : obj.fields) {
        if (!meta::is_reference_type(f.type) || (f.system && f.name != "OwnerId")) continue;
        const FieldValue& v = r.get(f.name);
        if (v.kind() != ValueKind::String) continue;
        if (before != nullptr && before->get(f.name) == v) continue;
        const std::string& id = v.as_string();
        if (f.reference_to == "User") {
            const auth::User* u = org.users.find(id);
            if (u == nullptr || !u->active) {
                throw errors::bad_request("INVALID_CROSS_REFERENCE_KEY",
                                          f.label + ": id value of incorrect type or inactive user: " + id, {f.name});
            }
            continue;
        }
        const ObjectDef* target = nullptr;
        std::optional<Record> ref = load_record(org, id, &target);
        if (!ref || ref->is_deleted() || target == nullptr || !reference_allowed(f, target->name)) {
            throw errors::bad_request("INVALID_CROSS_REFERENCE_KEY", "invalid cross reference id", {f.name});
        }
    }
}

void RecordService::apply_defaults(const Org& org, const Principal& who, const ObjectDef& obj, Record* r) const {
    if (obj.field("OwnerId") != nullptr && r->get("OwnerId").is_null() && !who.user_id.empty()) {
        r->set("OwnerId", FieldValue::string(who.user_id));
    }
    for (const FieldDef& f : obj.fields) {
        if (f.is_calculated() || f.system || !r->get(f.name).is_null()) continue;
        if (f.type == FieldType::Picklist) {
            for (const meta::PicklistValue& p : f.picklist) {
                if (p.is_default && p.active) r->set(f.name, FieldValue::string(p.value));
            }
            continue;
        }
        if (f.type == FieldType::Checkbox) {
            r->set(f.name, FieldValue::boolean(f.default_value == "true"));
            continue;
        }
        if (!f.default_value.empty()) {
            const FieldValue v = evaluate_formula(f.default_value, org, obj, *r, nullptr, who);
            if (!v.is_null()) r->set(f.name, meta::value_from_json(f, json::Value(v.to_text())));
        }
    }
}

void RecordService::assign_autonumbers(Org& org, const ObjectDef& obj, Record* r) const {
    for (const FieldDef& f : obj.fields) {
        if (f.type != FieldType::AutoNumber) continue;
        const int64_t n = org.next_autonumber(obj.name + "." + f.name);
        r->set(f.name, FieldValue::string(format_autonumber(f.autonumber_format, n)));
        org.save_counters();
    }
}

void RecordService::compute_formulas(const Org& org, const Principal& who, const ObjectDef& obj, Record* r,
                                     const Record* before) const {
    for (const FieldDef& f : obj.fields) {
        if (f.type != FieldType::Formula) continue;
        try {
            const FieldValue v = evaluate_formula(f.formula, org, obj, *r, before, who);
            r->set(f.name, coerce_formula_result(f, v));
        } catch (const ApiError& e) {
            log::warn("formula", org.id(), " ", obj.name, ".", f.name, ": ", e.what());
            r->set(f.name, FieldValue::null());
        }
    }
}

void RecordService::check_required(const ObjectDef& obj, const Record& r) const {
    std::vector<std::string> missing;
    for (const FieldDef& f : obj.fields) {
        if (!f.required || f.is_calculated() || f.system) continue;
        const FieldValue& v = r.get(f.name);
        if (v.is_null() || (v.kind() == ValueKind::String && str::trim(v.as_string()).empty())) {
            missing.push_back(f.name);
        }
    }
    if (!missing.empty()) {
        throw errors::bad_request("REQUIRED_FIELD_MISSING",
                                  "Required fields are missing: [" + str::join(missing, ", ") + "]", missing);
    }
}

void RecordService::check_unique(const Org& org, const ObjectDef& obj, const Record& r) const {
    const storage::Table* t = org.db.find_table(obj.name);
    if (t == nullptr) return;
    for (const FieldDef& f : obj.fields) {
        if (!f.unique && !f.external_id) continue;
        const FieldValue& v = r.get(f.name);
        if (v.is_null()) continue;
        const storage::Index* idx = t->index(f.name);
        if (idx == nullptr || !idx->taken_by_other(v, r.id)) continue;
        const std::vector<std::string> holders = idx->find(v);
        std::string other;
        for (const std::string& h : holders) {
            if (h != r.id) other = h;
        }
        throw errors::bad_request("DUPLICATE_VALUE",
                                  "duplicate value found: " + f.name + " duplicates value on record with id: " + other,
                                  {f.name});
    }
}

void RecordService::run_validation_rules(const Org& org, const Principal& who, const ObjectDef& obj, const Record& r,
                                         const Record* before) const {
    for (const ValidationRule& rule : org.automation.validation_rules) {
        if (!rule.active || !str::iequals(rule.object, obj.name)) continue;
        const FieldValue result = evaluate_formula(rule.condition, org, obj, r, before, who);
        if (formula::truthy(result)) {
            std::vector<std::string> fields;
            if (!rule.error_field.empty()) fields.push_back(rule.error_field);
            throw errors::bad_request("FIELD_CUSTOM_VALIDATION_EXCEPTION", rule.error_message, fields);
        }
    }
}

void RecordService::check_duplicates(const Org& org, const ObjectDef& obj, const Record& r) const {
    const storage::Table* t = org.db.find_table(obj.name);
    if (t == nullptr) return;
    if (obj.name == "Contact" || obj.name == "Lead") {
        const std::string email = str::to_lower(r.text("Email"));
        if (email.empty()) return;
        for (const auto& [id, other] : t->rows()) {
            if (id == r.id || other.is_deleted()) continue;
            if (str::to_lower(other.text("Email")) == email) {
                throw errors::bad_request("DUPLICATES_DETECTED",
                                          "You're creating a duplicate record. We recommend you use an existing "
                                          "record instead.",
                                          {"Email"});
            }
        }
    } else if (obj.name == "Account") {
        const std::string name = normalize_company(r.text("Name"));
        if (name.empty()) return;
        for (const auto& [id, other] : t->rows()) {
            if (id == r.id || other.is_deleted()) continue;
            if (normalize_company(other.text("Name")) == name) {
                throw errors::bad_request("DUPLICATES_DETECTED",
                                          "You're creating a duplicate record. We recommend you use an existing "
                                          "record instead.",
                                          {"Name"});
            }
        }
    }
}

void RecordService::run_assignment_rules(Org& org, const Principal& who, const ObjectDef& obj, Record* r) const {
    std::vector<const AssignmentRule*> rules;
    for (const AssignmentRule& rule : org.automation.assignment_rules) {
        if (rule.active && str::iequals(rule.object, obj.name) && !rule.user_ids.empty()) rules.push_back(&rule);
    }
    std::stable_sort(rules.begin(), rules.end(),
                     [](const AssignmentRule* a, const AssignmentRule* b) { return a->sort_order < b->sort_order; });
    static size_t s_round_robin = 0;
    for (const AssignmentRule* rule : rules) {
        if (!formula::truthy(evaluate_formula(rule->criteria, org, obj, *r, nullptr, who))) continue;
        const std::string& user_id = rule->user_ids[s_round_robin++ % rule->user_ids.size()];
        const auth::User* u = org.users.find(user_id);
        if (u != nullptr && u->active) {
            r->set("OwnerId", FieldValue::string(u->id));
            return;
        }
    }
}

void RecordService::track_history(Org& org, const Principal& who, const ObjectDef& obj, const Record& before,
                                  const Record& after) const {
    if (!obj.track_history) return;
    const int64_t now = timeutil::now_ms();
    for (const FieldDef& f : obj.fields) {
        if (!f.history_tracked) continue;
        const FieldValue& a = before.get(f.name);
        const FieldValue& b = after.get(f.name);
        if (a == b) continue;
        Record h;
        h.id = org.new_id("017");
        h.object = kHistoryTable;
        h.set("ParentId", FieldValue::string(after.id));
        h.set("Object", FieldValue::string(obj.name));
        h.set("Field", FieldValue::string(f.name));
        h.set("OldValue", FieldValue::string(history_value(f, a).dump()));
        h.set("NewValue", FieldValue::string(history_value(f, b).dump()));
        h.set("CreatedById", FieldValue::string(who.user_id));
        h.set("CreatedDate", FieldValue::datetime(now));
        org.db.put(h);
    }
}

void RecordService::update_rollups(Org& org, const ObjectDef& child_obj, const Record& child) const {
    for (const ObjectDef* parent_obj : org.schema.objects()) {
        for (const FieldDef& f : parent_obj->fields) {
            if (f.type != FieldType::RollupSummary || f.rollup.child_object != child_obj.name) continue;
            const FieldValue& link = child.get(f.rollup.relationship_field);
            if (link.kind() != ValueKind::String) continue;
            const std::string parent_id = link.as_string();
            storage::Table& parents = org.db.table(parent_obj->name);
            const Record* parent = parents.get(parent_id);
            if (parent == nullptr) continue;

            const storage::Table* children = org.db.find_table(child_obj.name);
            std::vector<std::string> child_ids;
            if (const storage::Index* idx = children->index(f.rollup.relationship_field)) {
                child_ids = idx->find(link);
            }
            const std::string target_currency =
                parent->text("CurrencyIsoCode").empty() ? org.settings.currency : parent->text("CurrencyIsoCode");
            int64_t count = 0;
            double sum = 0;
            FieldValue min, max;
            for (const std::string& cid : child_ids) {
                const Record* c = children->get(cid);
                if (c == nullptr || c->is_deleted()) continue;
                ++count;
                if (f.rollup.op == "COUNT") continue;
                const FieldValue& v = c->get(f.rollup.field);
                if (v.is_null()) continue;
                if (v.is_numeric()) {
                    double amount = v.as_double();
                    const FieldDef* src = child_obj.field(f.rollup.field);
                    if (src != nullptr && src->value_type() == FieldType::Currency) {
                        const std::string from =
                            c->text("CurrencyIsoCode").empty() ? org.settings.currency : c->text("CurrencyIsoCode");
                        amount = convert_currency(org.settings, amount, from, target_currency);
                    }
                    sum += amount;
                    if (min.is_null() || amount < min.as_double()) min = FieldValue::number(amount);
                    if (max.is_null() || amount > max.as_double()) max = FieldValue::number(amount);
                } else {
                    if (min.is_null() || v < min) min = v;
                    if (max.is_null() || max < v) max = v;
                }
            }
            FieldValue result;
            if (f.rollup.op == "COUNT") result = FieldValue::integer(count);
            else if (f.rollup.op == "SUM") result = FieldValue::number(sum);
            else if (f.rollup.op == "MIN") result = min;
            else result = max;
            if (parent->get(f.name) == result) continue;
            Record updated = *parent;
            updated.set(f.name, result);
            updated.set(sysfield::kSystemModstamp, FieldValue::datetime(timeutil::now_ms()));
            ++updated.version;
            org.db.put(updated);
        }
    }
}

void RecordService::run_workflows(Org& org, const Principal& who, const ObjectDef& obj, const Record& r,
                                  const Record* before) {
    if (t_workflow_depth >= kMaxWorkflowDepth) {
        log::warn("workflow", org.id(), ": re-evaluation depth exceeded on ", r.id);
        return;
    }
    WorkflowDepthGuard{t_workflow_depth};

    const bool created = before == nullptr;
    json::Value updates = json::Value::object();
    for (const WorkflowRule& rule : org.automation.workflow_rules) {
        if (!rule.active || !str::iequals(rule.object, obj.name)) continue;
        if (created ? !rule.on_create : !rule.on_update) continue;
        if (!formula::truthy(evaluate_formula(rule.criteria, org, obj, r, before, who))) continue;
        if (!created && rule.only_when_newly_true &&
            formula::truthy(evaluate_formula(rule.criteria, org, obj, *before, nullptr, who))) {
            continue;
        }
        for (const WorkflowAction& action : rule.actions) {
            switch (action.type) {
                case WorkflowAction::Type::FieldUpdate: {
                    const FieldDef* f = obj.field(action.field);
                    if (f == nullptr || !f->is_createable()) {
                        log::warn("workflow", rule.name, ": can't update field ", action.field);
                        break;
                    }
                    const FieldValue v = evaluate_formula(action.value_formula, org, obj, r, before, who);
                    if (v == r.get(f->name)) break;
                    json::Value jv;
                    if (v.kind() == ValueKind::Bool) jv = json::Value(v.as_bool());
                    else if (v.is_numeric()) jv = json::Value(v.as_double());
                    else if (!v.is_null()) jv = json::Value(v.to_text());
                    updates.set(f->name, std::move(jv));
                    break;
                }
                case WorkflowAction::Type::CreateTask: {
                    json::Value task = json::Value::object();
                    task.set("Subject", action.subject.empty() ? rule.name : action.subject);
                    task.set("ActivityDate",
                             timeutil::format_date(org.tz().local_date(timeutil::now_ms()) + action.due_in_days));
                    std::string owner = r.owner_id();
                    if (action.recipient == "creator") owner = r.text("CreatedById");
                    else if (action.recipient != "owner") owner = action.recipient;
                    if (!owner.empty()) task.set("OwnerId", owner);
                    const ObjectDef& task_obj = require_object(org, "Task");
                    const FieldDef* what = task_obj.field("WhatId");
                    if (reference_allowed(*what, obj.name)) task.set("WhatId", r.id);
                    else if (reference_allowed(*task_obj.field("WhoId"), obj.name)) task.set("WhoId", r.id);
                    SaveOptions opts;
                    opts.run_workflows = false;
                    create_locked(org, Principal::system_for(who.org, who.user_id), task_obj, task, opts);
                    break;
                }
                case WorkflowAction::Type::Webhook: {
                    if (hooks_ == nullptr) break;
                    json::Value payload = json::Value::object();
                    payload.set("event", created ? "created" : "updated");
                    payload.set("rule", rule.name);
                    payload.set("object", obj.name);
                    const Principal system = Principal::system_for(who.org);
                    AccessChecker sys(org, system);
                    payload.set("record", record_to_api_json(obj, r, sys));
                    hooks_->webhook(org, action.webhook_id, payload);
                    break;
                }
                case WorkflowAction::Type::EmailAlert: {
                    if (hooks_ == nullptr) break;
                    std::string to = r.owner_id();
                    if (action.recipient == "creator") to = r.text("CreatedById");
                    else if (action.recipient != "owner") to = action.recipient;
                    hooks_->email(org, to, action.subject.empty() ? rule.name : action.subject,
                                  obj.label + " " + record_name(obj, r) + " matched workflow rule " + rule.name);
                    break;
                }
            }
        }
    }
    if (updates.size() > 0) {
        SaveOptions opts;
        update_locked(org, Principal::system_for(who.org, who.user_id), obj, r.id, updates, opts);
    }
}

// ---- save pipeline ----------------------------------------------------------

Record RecordService::save(Org& org, const Principal& who, const ObjectDef& obj, Record draft, const Record* before,
                           const SaveOptions& options) {
    const int64_t now = timeutil::now_ms();
    if (before == nullptr) {
        draft.set(sysfield::kCreatedDate, FieldValue::datetime(now));
        draft.set(sysfield::kCreatedById, FieldValue::string(who.user_id));
        draft.set(sysfield::kIsDeleted, FieldValue::boolean(false));
    }
    draft.set(sysfield::kLastModifiedDate, FieldValue::datetime(now));
    draft.set(sysfield::kLastModifiedById, FieldValue::string(who.user_id));
    draft.set(sysfield::kSystemModstamp, FieldValue::datetime(now));

    compute_formulas(org, who, obj, &draft, before);
    check_required(obj, draft);
    check_unique(org, obj, draft);
    run_validation_rules(org, who, obj, draft, before);
    if (!options.allow_duplicates && org.settings.duplicate_rules) {
        const bool matching_changed = before == nullptr || before->get("Email") != draft.get("Email") ||
                                      before->get("Name") != draft.get("Name");
        if (matching_changed) check_duplicates(org, obj, draft);
    }

    draft.version = before == nullptr ? 1 : before->version + 1;
    org.db.put(draft);
    if (before != nullptr) track_history(org, who, obj, *before, draft);
    update_rollups(org, obj, draft);
    if (hooks_) hooks_->saved(org, obj, before, draft);
    if (options.run_workflows) run_workflows(org, who, obj, draft, before);
    const Record* stored = org.db.table(obj.name).get(draft.id);
    return stored ? *stored : draft;
}

Record RecordService::create_locked(Org& org, const Principal& who, const ObjectDef& obj, const json::Value& fields,
                                    const SaveOptions& options) {
    AccessChecker access(org, who);
    access.require_object(obj, ObjectOp::Create);
    if (obj.name == "User") throw errors::bad_request("INVALID_TYPE", "Use the users API to create users");
    Record draft;
    draft.object = obj.name;
    draft.id = org.new_id(obj.key_prefix);
    draft.set("Id", FieldValue::string(draft.id));
    apply_fields(org, access, obj, fields, Mode::Create, &draft);
    const bool owner_given = !draft.get("OwnerId").is_null();
    apply_defaults(org, who, obj, &draft);
    check_references(org, obj, draft, nullptr);
    assign_autonumbers(org, obj, &draft);
    if (options.run_assignment_rules && !owner_given && (obj.name == "Lead" || obj.name == "Case")) {
        run_assignment_rules(org, who, obj, &draft);
    }
    apply_standard_behavior(org, obj, &draft, nullptr, fields);
    return save(org, who, obj, std::move(draft), nullptr, options);
}

Record RecordService::create(const Principal& who, const std::string& object, const json::Value& fields,
                             const SaveOptions& options) {
    Org& org = *who.org;
    Record draft;
    const ObjectDef* obj = nullptr;
    bool owner_given = false;
    {
        // Build and validate the draft under a shared lock so that expensive
        // default-value formulas and reference checks don't block readers.
        std::shared_lock<std::shared_mutex> lock(org.mutex());
        obj = &require_object(org, object);
        AccessChecker access(org, who);
        access.require_object(*obj, ObjectOp::Create);
        if (obj->name == "User") throw errors::bad_request("INVALID_TYPE", "Use the users API to create users");
        draft.object = obj->name;
        draft.id = org.new_id(obj->key_prefix);
        draft.set("Id", FieldValue::string(draft.id));
        apply_fields(org, access, *obj, fields, Mode::Create, &draft);
        owner_given = !draft.get("OwnerId").is_null();
        apply_defaults(org, who, *obj, &draft);
        check_references(org, *obj, draft, nullptr);
        assign_autonumbers(org, *obj, &draft);
    }
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    obj = &require_object(org, object);
    if (options.run_assignment_rules && !owner_given && (obj->name == "Lead" || obj->name == "Case")) {
        run_assignment_rules(org, who, *obj, &draft);
    }
    apply_standard_behavior(org, *obj, &draft, nullptr, fields);
    return save(org, who, *obj, std::move(draft), nullptr, options);
}

Record RecordService::update_locked(Org& org, const Principal& who, const ObjectDef& obj, const std::string& id,
                                    const json::Value& fields, const SaveOptions& options) {
    AccessChecker access(org, who);
    access.require_object(obj, ObjectOp::Edit);
    std::string id18;
    if (!ids::normalize(id, &id18)) throw errors::bad_request("MALFORMED_ID", "Malformed id: " + id);
    const storage::Table* table = org.db.find_table(obj.name);
    const Record* existing = table ? table->get(id18) : nullptr;
    if (existing == nullptr) throw errors::not_found();
    if (existing->is_deleted()) throw errors::entity_deleted();
    if (!access.can_edit(obj, *existing)) throw errors::forbidden();
    if (!who.system && !who.profile.modify_all_data && record_locked(org, id18)) {
        throw errors::bad_request("ENTITY_IS_LOCKED", "This record is locked while it is pending approval");
    }
    if (options.expected_version != 0 && existing->version != options.expected_version) {
        throw ApiError(412, "PRECONDITION_FAILED", "The record was modified by someone else");
    }
    const Record before = *existing;
    Record draft = before;
    apply_fields(org, access, obj, fields, Mode::Update, &draft);
    if (draft.get("OwnerId") != before.get("OwnerId") && !access.can_delete(obj, before)) {
        throw errors::forbidden("Only the owner, their managers or an administrator can transfer this record");
    }
    check_references(org, obj, draft, &before);
    apply_standard_behavior(org, obj, &draft, &before, fields);
    return save(org, who, obj, std::move(draft), &before, options);
}

Record RecordService::update(const Principal& who, const std::string& object, const std::string& id,
                             const json::Value& fields, const SaveOptions& options) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    return update_locked(org, who, require_object(org, object), id, fields, options);
}

RecordService::UpsertResult RecordService::upsert(const Principal& who, const std::string& object,
                                                  const std::string& external_field, const std::string& external_value,
                                                  const json::Value& fields, const SaveOptions& options) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    const ObjectDef& obj = require_object(org, object);
    const FieldDef* key = obj.field(external_field);
    if (key == nullptr) throw errors::not_found("Provided external ID field does not exist or is not accessible: " + external_field);
    if (key->name == "Id") {
        UpsertResult r;
        r.record = update_locked(org, who, obj, external_value, fields, options);
        return r;
    }
    if (!key->external_id && !key->unique) {
        throw errors::bad_request("INVALID_FIELD", "Field " + key->name + " is not an external id or unique field");
    }
    const FieldValue lookup = meta::value_from_json(*key, json::Value(external_value));
    const storage::Table& table = org.db.table(obj.name);
    std::vector<std::string> matches;
    if (const storage::Index* idx = table.index(key->name)) matches = idx->find(lookup);
    matches.erase(std::remove_if(matches.begin(), matches.end(),
                                 [&](const std::string& mid) {
                                     const Record* r = table.get(mid);
                                     return r == nullptr || r->is_deleted();
                                 }),
                  matches.end());
    if (matches.size() > 1) {
        throw ApiError(300, "MULTIPLE_CHOICES", "More than one record found for external id field " + key->name);
    }
    UpsertResult result;
    if (matches.empty()) {
        json::Value with_key = fields;
        with_key.set(key->name, json::Value(external_value));
        result.record = create_locked(org, who, obj, with_key, options);
        result.created = true;
        return result;
    }

    AccessChecker access(org, who);
    access.require_object(obj, ObjectOp::Edit);
    const Record* existing = table.get(matches[0]);
    if (!access.can_edit(obj, *existing)) throw errors::forbidden();
    const Record before = *existing;
    Record draft = before;
    apply_fields(org, access, obj, fields, Mode::Upsert, &draft);
    check_references(org, obj, draft, &before);
    apply_standard_behavior(org, obj, &draft, &before, fields);
    result.record = save(org, who, obj, std::move(draft), &before, options);
    return result;
}

// ---- delete / undelete --------------------------------------------------------

void RecordService::remove_locked(Org& org, const Principal& who, const ObjectDef& obj, const std::string& id) {
    AccessChecker access(org, who);
    access.require_object(obj, ObjectOp::Delete);
    std::string id18;
    if (!ids::normalize(id, &id18)) throw errors::bad_request("MALFORMED_ID", "Malformed id: " + id);
    storage::Table& table = org.db.table(obj.name);
    const Record* existing = table.get(id18);
    if (existing == nullptr) throw errors::not_found();
    if (existing->is_deleted()) throw errors::entity_deleted();
    if (!access.can_delete(obj, *existing)) throw errors::forbidden();
    if (!who.system && !who.profile.modify_all_data && record_locked(org, id18)) {
        throw errors::bad_request("ENTITY_IS_LOCKED", "This record is locked while it is pending approval");
    }

    for (const meta::ChildRelationship& rel : obj.children) {
        if (rel.delete_rule != meta::DeleteRule::Restrict) continue;
        const storage::Table* ct = org.db.find_table(rel.child_object);
        if (ct == nullptr) continue;
        for (const auto& [cid, child] : ct->rows()) {
            if (!child.is_deleted() && child.text(rel.field) == id18) {
                throw errors::bad_request("DELETE_FAILED", "Your attempt to delete " + record_name(obj, *existing) +
                                                               " could not be completed because it is associated with " +
                                                               "other records");
            }
        }
    }

    const std::string batch = existing->text(kDeleteBatch).empty() ? org.new_id("0DB") : existing->text(kDeleteBatch);
    const int64_t now = timeutil::now_ms();
    std::vector<std::pair<const ObjectDef*, std::string>> pending = {{&obj, id18}};
    while (!pending.empty()) {
        auto [cur_obj, cur_id] = pending.back();
        pending.pop_back();
        storage::Table& t = org.db.table(cur_obj->name);
        const Record* rec = t.get(cur_id);
        if (rec == nullptr || rec->is_deleted()) continue;
        Record deleted = *rec;
        deleted.set(sysfield::kIsDeleted, FieldValue::boolean(true));
        deleted.set(sysfield::kLastModifiedDate, FieldValue::datetime(now));
        deleted.set(sysfield::kLastModifiedById, FieldValue::string(who.user_id));
        deleted.set(kDeleteBatch, FieldValue::string(batch));
        ++deleted.version;
        org.db.put(deleted);
        if (hooks_) hooks_->deleted(org, *cur_obj, deleted);

        for (const meta::ChildRelationship& rel : cur_obj->children) {
            const ObjectDef* child_obj = org.schema.object(rel.child_object);
            storage::Table* ct = &org.db.table(rel.child_object);
            std::vector<std::string> child_ids;
            if (const storage::Index* idx = ct->index(rel.field)) {
                child_ids = idx->find(FieldValue::string(cur_id));
            }
            for (const std::string& cid : child_ids) {
                const Record* child = ct->get(cid);
                if (child == nullptr || child->is_deleted()) continue;
                if (rel.delete_rule == meta::DeleteRule::Cascade) {
                    pending.emplace_back(child_obj, cid);
                } else if (rel.delete_rule == meta::DeleteRule::SetNull) {
                    Record updated = *child;
                    updated.set(rel.field, FieldValue::null());
                    ++updated.version;
                    org.db.put(updated);
                }
            }
        }
    }
}

void RecordService::remove(const Principal& who, const std::string& object, const std::string& id) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    remove_locked(org, who, require_object(org, object), id);
}

Record RecordService::undelete(const Principal& who, const std::string& id) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    std::string id18;
    if (!ids::normalize(id, &id18)) throw errors::bad_request("MALFORMED_ID", "Malformed id: " + id);
    const ObjectDef* obj = object_for_id(org, id18);
    if (obj == nullptr) throw errors::not_found();
    storage::Table& table = org.db.table(obj->name);
    const Record* rec = table.get(id18);
    if (rec == nullptr) throw errors::not_found();
    if (!rec->is_deleted()) throw errors::bad_request("UNDELETE_FAILED", "Entity is not in the recycle bin");
    AccessChecker access(org, who);
    if (!access.can_delete(*obj, *rec)) throw errors::forbidden();

    const std::string batch = rec->text(kDeleteBatch);
    Record restored;
    for (const ObjectDef* o : org.schema.objects()) {
        storage::Table* t = &org.db.table(o->name);
        std::vector<Record> to_restore;
        for (const auto& [rid, r] : t->rows()) {
            if (r.is_deleted() && (rid == id18 || (!batch.empty() && r.text(kDeleteBatch) == batch))) {
                to_restore.push_back(r);
            }
        }
        for (Record& r : to_restore) {
            r.set(sysfield::kIsDeleted, FieldValue::boolean(false));
            r.fields.erase(kDeleteBatch);
            r.set(sysfield::kLastModifiedDate, FieldValue::datetime(timeutil::now_ms()));
            ++r.version;
            org.db.put(r);
            if (hooks_) hooks_->undeleted(org, *o, r);
            if (r.id == id18) restored = r;
        }
    }
    return restored;
}

void RecordService::purge(const Principal& who, const std::string& id) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    std::string id18;
    if (!ids::normalize(id, &id18)) throw errors::bad_request("MALFORMED_ID", "Malformed id: " + id);
    const ObjectDef* obj = object_for_id(org, id18);
    if (obj == nullptr) throw errors::not_found();
    const Record* rec = org.db.table(obj->name).get(id18);
    if (rec == nullptr) throw errors::not_found();
    if (!rec->is_deleted()) throw errors::bad_request("PURGE_FAILED", "Only records in the recycle bin can be purged");
    AccessChecker access(org, who);
    if (!access.can_delete(*obj, *rec)) throw errors::forbidden();
    org.db.erase(obj->name, id18);
}

size_t RecordService::purge_expired(Org& org, int64_t now_ms) {
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    const int64_t cutoff = now_ms - static_cast<int64_t>(org.settings.recycle_bin_days) * timeutil::kMsPerDay;
    size_t purged = 0;
    for (const std::string& name : org.db.table_names()) {
        auto& rows = org.db.table(name).mutable_rows();
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            const Record& r = it->second;
            if (!r.is_deleted()) continue;
            const FieldValue& when = r.get(sysfield::kLastModifiedDate);
            if (when.kind() == ValueKind::DateTime && when.as_datetime() < cutoff) {
                org.db.erase(name, it->first);
                ++purged;
            }
        }
    }
    if (purged > 0) log::info("recyclebin", org.id(), ": purged ", purged, " records");
    return purged;
}

// ---- reads --------------------------------------------------------------------

Record RecordService::get(const Principal& who, const std::string& object, const std::string& id) {
    Org& org = *who.org;
    std::shared_lock<std::shared_mutex> lock(org.mutex());
    const ObjectDef& obj = require_object(org, object);
    AccessChecker access(org, who);
    access.require_object(obj, ObjectOp::Read);
    std::string id18;
    if (!ids::normalize(id, &id18)) throw errors::bad_request("MALFORMED_ID", "Malformed id: " + id);
    if (ids::key_prefix(id18) != obj.key_prefix) throw errors::not_found();
    std::optional<Record> r = load_record(org, id18);
    if (!r) throw errors::not_found();
    if (r->is_deleted()) throw errors::entity_deleted();
    if (!access.can_read(obj, *r)) throw errors::not_found();
    return *r;
}

std::vector<HistoryEntry> RecordService::history(const Principal& who, const std::string& object,
                                                 const std::string& id) {
    const Record r = get(who, object, id);
    Org& org = *who.org;
    std::shared_lock<std::shared_mutex> lock(org.mutex());
    std::vector<HistoryEntry> out;
    const storage::Table* t = org.db.find_table(kHistoryTable);
    if (t == nullptr) return out;
    for (const auto& [hid, h] : t->rows()) {
        if (h.text("ParentId") != r.id) continue;
        HistoryEntry e;
        e.id = hid;
        e.record_id = r.id;
        e.object = h.text("Object");
        e.field = h.text("Field");
        json::try_parse(h.text("OldValue"), &e.old_value);
        json::try_parse(h.text("NewValue"), &e.new_value);
        e.changed_by = h.text("CreatedById");
        const FieldValue& when = h.get("CreatedDate");
        e.changed_at = when.kind() == ValueKind::DateTime ? when.as_datetime() : 0;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
        return a.changed_at != b.changed_at ? a.changed_at > b.changed_at : a.id > b.id;
    });
    return out;
}

}  // namespace tcrm::engine
