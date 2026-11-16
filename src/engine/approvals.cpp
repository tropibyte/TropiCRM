// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/approvals.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "engine/formula_context.hpp"
#include "engine/records.hpp"
#include "formula/formula.hpp"
#include "model/errors.hpp"
#include "util/id.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::engine {
namespace {

ApprovalStatus parse_status(const std::string& s) {
    if (s == "Approved") return ApprovalStatus::Approved;
    if (s == "Rejected") return ApprovalStatus::Rejected;
    if (s == "Recalled") return ApprovalStatus::Recalled;
    return ApprovalStatus::Pending;
}

json::Value outcome_value(const FieldValue& v) {
    if (v.is_null()) return json::Value();
    if (v.kind() == ValueKind::Bool) return json::Value(v.as_bool());
    if (v.is_numeric()) return json::Value(v.as_double());
    return json::Value(v.to_text());
}

}  // namespace

const char* approval_status_name(ApprovalStatus s) {
    switch (s) {
        case ApprovalStatus::Pending: return "Pending";
        case ApprovalStatus::Approved: return "Approved";
        case ApprovalStatus::Rejected: return "Rejected";
        case ApprovalStatus::Recalled: return "Recalled";
    }
    return "Pending";
}

json::Value ApprovalProcess::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("object", object);
    v.set("entryCriteria", entry_criteria);
    json::Value js = json::Value::array();
    for (const ApprovalStep& s : steps) {
        json::Value step = json::Value::object();
        step.set("name", s.name);
        step.set("approver", s.approver);
        js.push_back(std::move(step));
    }
    v.set("steps", std::move(js));
    v.set("approvedField", approved_field);
    v.set("approvedValue", approved_value);
    v.set("rejectedField", rejected_field);
    v.set("rejectedValue", rejected_value);
    v.set("active", active);
    return v;
}

ApprovalProcess ApprovalProcess::from_json(const json::Value& v) {
    ApprovalProcess p;
    p.id = v.get_string("id");
    p.name = v.get_string("name");
    p.object = v.get_string("object");
    p.entry_criteria = v.get_string("entryCriteria", "true");
    if (const json::Value* steps = v.find("steps"); steps && steps->is_array()) {
        for (const json::Value& s : steps->as_array()) {
            ApprovalStep step;
            step.name = s.get_string("name");
            step.approver = s.get_string("approver", "manager");
            p.steps.push_back(std::move(step));
        }
    }
    p.approved_field = v.get_string("approvedField");
    p.approved_value = v.get_string("approvedValue");
    p.rejected_field = v.get_string("rejectedField");
    p.rejected_value = v.get_string("rejectedValue");
    p.active = v.get_bool("active", true);
    return p;
}

json::Value ApprovalRequest::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("processId", process_id);
    v.set("object", object);
    v.set("recordId", record_id);
    v.set("submitterId", submitter_id);
    v.set("currentApproverId", current_approver);
    v.set("step", step);
    v.set("status", approval_status_name(status));
    json::Value hist = json::Value::array();
    for (const ApprovalAction& a : history) {
        json::Value h = json::Value::object();
        h.set("actorId", a.actor_id);
        h.set("action", a.action);
        h.set("comments", a.comments);
        h.set("date", timeutil::format_datetime(a.at));
        hist.push_back(std::move(h));
    }
    v.set("history", std::move(hist));
    v.set("createdDate", timeutil::format_datetime(created_at));
    v.set("lastModifiedDate", timeutil::format_datetime(updated_at));
    return v;
}

ApprovalRequest ApprovalRequest::from_json(const json::Value& v) {
    ApprovalRequest r;
    r.id = v.get_string("id");
    r.process_id = v.get_string("processId");
    r.object = v.get_string("object");
    r.record_id = v.get_string("recordId");
    r.submitter_id = v.get_string("submitterId");
    r.current_approver = v.get_string("currentApproverId");
    r.step = static_cast<size_t>(v.get_int("step"));
    r.status = parse_status(v.get_string("status"));
    if (const json::Value* hist = v.find("history"); hist && hist->is_array()) {
        for (const json::Value& h : hist->as_array()) {
            ApprovalAction a;
            a.actor_id = h.get_string("actorId");
            a.action = h.get_string("action");
            a.comments = h.get_string("comments");
            timeutil::parse_datetime(h.get_string("date"), &a.at);
            r.history.push_back(std::move(a));
        }
    }
    timeutil::parse_datetime(v.get_string("createdDate"), &r.created_at);
    timeutil::parse_datetime(v.get_string("lastModifiedDate"), &r.updated_at);
    return r;
}

std::vector<ApprovalProcess> load_approval_processes(const Org& org) {
    std::vector<ApprovalProcess> out;
    if (const json::Value* v = org.db.meta("approvalProcesses"); v && v->is_array()) {
        for (const json::Value& p : v->as_array()) out.push_back(ApprovalProcess::from_json(p));
    }
    return out;
}

void store_approval_processes(Org& org, const std::vector<ApprovalProcess>& processes) {
    json::Value arr = json::Value::array();
    for (const ApprovalProcess& p : processes) arr.push_back(p.to_json());
    org.db.put_meta("approvalProcesses", arr);
}

std::vector<ApprovalRequest> load_approval_requests(const Org& org) {
    std::vector<ApprovalRequest> out;
    if (const json::Value* v = org.db.meta("approvalRequests"); v && v->is_array()) {
        for (const json::Value& r : v->as_array()) out.push_back(ApprovalRequest::from_json(r));
    }
    return out;
}

void store_approval_requests(Org& org, const std::vector<ApprovalRequest>& requests) {
    json::Value arr = json::Value::array();
    for (const ApprovalRequest& r : requests) arr.push_back(r.to_json());
    org.db.put_meta("approvalRequests", arr);
}

void validate_approval_process(const Org& org, ApprovalProcess* p) {
    if (str::trim(p->name).empty()) throw errors::invalid_field("name", "Process name is required");
    const meta::ObjectDef* obj = org.schema.object(p->object);
    if (obj == nullptr || obj->name == "User") throw errors::invalid_field("object", "Unsupported object " + p->object);
    p->object = obj->name;
    if (p->steps.empty()) throw errors::invalid_field("steps", "A process needs at least one step");
    for (const ApprovalStep& s : p->steps) {
        if (s.approver != "manager" && org.users.find(s.approver) == nullptr) {
            throw errors::invalid_field("steps", "Approver must be \"manager\" or a user id");
        }
    }
    auto check = [&](const std::string& field, const std::string& value, const char* what) {
        if (field.empty()) return;
        const meta::FieldDef* f = obj->field(field);
        if (f == nullptr || !f->is_createable()) throw errors::invalid_field(what, "Can't update field " + field);
        try {
            formula::Formula::compile(value);
        } catch (const formula::FormulaError& e) {
            throw errors::invalid_field(what, e.what());
        }
    };
    check(p->approved_field, p->approved_value, "approvedField");
    check(p->rejected_field, p->rejected_value, "rejectedField");
    try {
        formula::Formula::compile(p->entry_criteria);
    } catch (const formula::FormulaError& e) {
        throw errors::invalid_field("entryCriteria", e.what());
    }
}

bool record_locked(const Org& org, const std::string& record_id) {
    const json::Value* v = org.db.meta("approvalRequests");
    if (v == nullptr || !v->is_array()) return false;
    for (const json::Value& r : v->as_array()) {
        if (r.get_string("recordId") == record_id && r.get_string("status") == "Pending") return true;
    }
    return false;
}

std::string ApprovalService::resolve_approver(const Org& org, const ApprovalStep& step, const std::string& submitter) {
    if (step.approver != "manager") return step.approver;
    const auth::User* u = org.users.find(submitter);
    if (u != nullptr && !u->manager_id.empty()) return u->manager_id;
    // No manager on file: route the request back to the submitter rather
    // than leaving it stuck with nobody able to act on it.
    return submitter;
}

ApprovalRequest ApprovalService::submit(const Principal& who, const std::string& record_id,
                                        const std::string& comments) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    std::string id18;
    if (!ids::normalize(record_id, &id18)) throw errors::bad_request("MALFORMED_ID", "Malformed record id");
    const meta::ObjectDef* obj = nullptr;
    std::optional<Record> rec = load_record(org, id18, &obj);
    if (!rec || rec->is_deleted()) throw errors::not_found();
    AccessChecker access(org, who);
    if (!access.can_edit(*obj, *rec)) throw errors::forbidden();
    if (record_locked(org, id18)) throw errors::bad_request("ALREADY_IN_PROCESS", "This record is already pending approval");

    for (const ApprovalProcess& p : load_approval_processes(org)) {
        if (!p.active || p.object != obj->name) continue;
        if (!formula::truthy(evaluate_formula(p.entry_criteria, org, *obj, *rec, nullptr, who))) continue;
        ApprovalRequest r;
        r.id = org.new_id("04g");
        r.process_id = p.id;
        r.object = obj->name;
        r.record_id = id18;
        r.submitter_id = who.user_id;
        r.current_approver = resolve_approver(org, p.steps[0], who.user_id);
        r.created_at = r.updated_at = timeutil::now_ms();
        r.history.push_back({who.user_id, "Submitted", comments, r.created_at});
        std::vector<ApprovalRequest> all = load_approval_requests(org);
        all.push_back(r);
        store_approval_requests(org, all);
        if (records_.hooks()) {
            records_.hooks()->email(org, r.current_approver, "Approval requested: " + record_name(*obj, *rec),
                                    obj->label + " " + record_name(*obj, *rec) + " needs your approval.");
        }
        return r;
    }
    throw errors::bad_request("NO_APPLICABLE_PROCESS", "No applicable approval process was found");
}

void ApprovalService::apply_outcome(Org& org, const Principal& who, const ApprovalProcess& p,
                                    const ApprovalRequest& r, bool approved) {
    const std::string& field = approved ? p.approved_field : p.rejected_field;
    const std::string& value = approved ? p.approved_value : p.rejected_value;
    if (field.empty()) return;
    const meta::ObjectDef* obj = org.schema.object(r.object);
    const Record* rec = org.db.table(r.object).get(r.record_id);
    if (obj == nullptr || rec == nullptr) return;
    const FieldValue v = evaluate_formula(value, org, *obj, *rec, nullptr, who);
    json::Value update = json::Value::object();
    update.set(field, outcome_value(v));
    SaveOptions opts;
    records_.update_locked(org, Principal::system_for(who.org, who.user_id), *obj, r.record_id, update, opts);
}

ApprovalRequest ApprovalService::act(const Principal& who, const std::string& request_id, const std::string& action,
                                     const std::string& comments) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    std::vector<ApprovalRequest> all = load_approval_requests(org);
    auto it = std::find_if(all.begin(), all.end(), [&](const ApprovalRequest& r) { return r.id == request_id; });
    if (it == all.end()) throw errors::not_found("No such approval request");
    ApprovalRequest& r = *it;
    if (r.status != ApprovalStatus::Pending) throw errors::bad_request("INVALID_STATUS", "The request is no longer pending");
    const std::vector<ApprovalProcess> processes = load_approval_processes(org);
    auto pit = std::find_if(processes.begin(), processes.end(), [&](const ApprovalProcess& p) { return p.id == r.process_id; });
    if (pit == processes.end()) throw errors::internal("approval process was deleted");
    const ApprovalProcess& p = *pit;
    const int64_t now = timeutil::now_ms();

    if (str::iequals(action, "Recall")) {
        if (r.submitter_id != who.user_id && !who.profile.modify_all_data) {
            throw errors::forbidden("Only the submitter or an administrator can recall a request");
        }
        r.status = ApprovalStatus::Recalled;
        r.history.push_back({who.user_id, "Recalled", comments, now});
    } else if (str::iequals(action, "Approve") || str::iequals(action, "Reject")) {
        if (r.current_approver != who.user_id && !who.profile.modify_all_data) {
            throw errors::forbidden("You are not the assigned approver");
        }
        const bool approve = str::iequals(action, "Approve");
        r.history.push_back({who.user_id, approve ? "Approved" : "Rejected", comments, now});
        if (!approve) {
            r.status = ApprovalStatus::Rejected;
        } else if (r.step + 1 < p.steps.size()) {
            ++r.step;
            r.current_approver = resolve_approver(org, p.steps[r.step], r.submitter_id);
        } else {
            r.status = ApprovalStatus::Approved;
        }
    } else {
        throw errors::invalid_field("action", "action must be Approve, Reject or Recall");
    }
    r.updated_at = now;
    const ApprovalRequest result = r;
    store_approval_requests(org, all);
    if (result.status == ApprovalStatus::Approved || result.status == ApprovalStatus::Rejected) {
        apply_outcome(org, who, p, result, result.status == ApprovalStatus::Approved);
        if (records_.hooks()) {
            records_.hooks()->email(org, result.submitter_id,
                                    std::string("Approval ") + approval_status_name(result.status),
                                    "Your approval request " + result.id + " was " +
                                        str::to_lower(approval_status_name(result.status)) + ".");
        }
    }
    log::info("approvals", org.id(), ": ", result.id, " ", action, " by ", who.user_id);
    return result;
}

std::vector<ApprovalRequest> ApprovalService::pending_for(const Principal& who) const {
    std::shared_lock<std::shared_mutex> lock(who.org->mutex());
    std::vector<ApprovalRequest> out;
    for (ApprovalRequest& r : load_approval_requests(*who.org)) {
        if (r.status == ApprovalStatus::Pending && r.current_approver == who.user_id) out.push_back(std::move(r));
    }
    return out;
}

std::vector<ApprovalRequest> ApprovalService::submitted_by(const Principal& who) const {
    std::shared_lock<std::shared_mutex> lock(who.org->mutex());
    std::vector<ApprovalRequest> out;
    for (ApprovalRequest& r : load_approval_requests(*who.org)) {
        if (r.submitter_id == who.user_id) out.push_back(std::move(r));
    }
    return out;
}

}  // namespace tcrm::engine
