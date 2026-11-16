// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/org.hpp"

#include <algorithm>
#include <filesystem>

#include "model/errors.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"

namespace fs = std::filesystem;

namespace tcrm::engine {
namespace {

const char* action_type_name(WorkflowAction::Type t) {
    switch (t) {
        case WorkflowAction::Type::FieldUpdate: return "FieldUpdate";
        case WorkflowAction::Type::CreateTask: return "CreateTask";
        case WorkflowAction::Type::Webhook: return "Webhook";
        case WorkflowAction::Type::EmailAlert: return "EmailAlert";
    }
    return "FieldUpdate";
}

WorkflowAction::Type parse_action_type(const std::string& s) {
    if (str::iequals(s, "CreateTask")) return WorkflowAction::Type::CreateTask;
    if (str::iequals(s, "Webhook") || str::iequals(s, "OutboundMessage")) return WorkflowAction::Type::Webhook;
    if (str::iequals(s, "EmailAlert")) return WorkflowAction::Type::EmailAlert;
    if (str::iequals(s, "FieldUpdate")) return WorkflowAction::Type::FieldUpdate;
    throw errors::invalid_field("type", "Unknown workflow action type: " + s);
}

}  // namespace

// ---- AutomationConfig ------------------------------------------------------

json::Value AutomationConfig::validation_rule_to_json(const ValidationRule& r) {
    json::Value v = json::Value::object();
    v.set("id", r.id);
    v.set("name", r.name);
    v.set("object", r.object);
    v.set("errorConditionFormula", r.condition);
    v.set("errorMessage", r.error_message);
    v.set("errorDisplayField", r.error_field);
    v.set("active", r.active);
    return v;
}

ValidationRule AutomationConfig::validation_rule_from_json(const json::Value& v) {
    ValidationRule r;
    r.id = v.get_string("id");
    r.name = v.get_string("name");
    r.object = v.get_string("object");
    r.condition = v.get_string("errorConditionFormula");
    r.error_message = v.get_string("errorMessage");
    r.error_field = v.get_string("errorDisplayField");
    r.active = v.get_bool("active", true);
    return r;
}

json::Value AutomationConfig::workflow_rule_to_json(const WorkflowRule& r) {
    json::Value v = json::Value::object();
    v.set("id", r.id);
    v.set("name", r.name);
    v.set("object", r.object);
    v.set("criteria", r.criteria);
    v.set("onCreate", r.on_create);
    v.set("onUpdate", r.on_update);
    v.set("onlyWhenNewlyTrue", r.only_when_newly_true);
    v.set("active", r.active);
    json::Value actions = json::Value::array();
    for (const WorkflowAction& a : r.actions) {
        json::Value ja = json::Value::object();
        ja.set("type", action_type_name(a.type));
        if (!a.field.empty()) ja.set("field", a.field);
        if (!a.value_formula.empty()) ja.set("value", a.value_formula);
        if (!a.subject.empty()) ja.set("subject", a.subject);
        if (a.due_in_days != 0) ja.set("dueInDays", a.due_in_days);
        if (!a.webhook_id.empty()) ja.set("webhookId", a.webhook_id);
        ja.set("recipient", a.recipient);
        actions.push_back(std::move(ja));
    }
    v.set("actions", std::move(actions));
    return v;
}

WorkflowRule AutomationConfig::workflow_rule_from_json(const json::Value& v) {
    WorkflowRule r;
    r.id = v.get_string("id");
    r.name = v.get_string("name");
    r.object = v.get_string("object");
    r.criteria = v.get_string("criteria", "true");
    r.on_create = v.get_bool("onCreate", true);
    r.on_update = v.get_bool("onUpdate", true);
    r.only_when_newly_true = v.get_bool("onlyWhenNewlyTrue");
    r.active = v.get_bool("active", true);
    if (const json::Value* actions = v.find("actions"); actions && actions->is_array()) {
        for (const json::Value& ja : actions->as_array()) {
            WorkflowAction a;
            a.type = parse_action_type(ja.get_string("type", "FieldUpdate"));
            a.field = ja.get_string("field");
            a.value_formula = ja.get_string("value");
            a.subject = ja.get_string("subject");
            a.due_in_days = static_cast<int>(ja.get_int("dueInDays"));
            a.webhook_id = ja.get_string("webhookId");
            a.recipient = ja.get_string("recipient", "owner");
            r.actions.push_back(std::move(a));
        }
    }
    return r;
}

json::Value AutomationConfig::assignment_rule_to_json(const AssignmentRule& r) {
    json::Value v = json::Value::object();
    v.set("id", r.id);
    v.set("name", r.name);
    v.set("object", r.object);
    v.set("criteria", r.criteria);
    json::Value users = json::Value::array();
    for (const std::string& u : r.user_ids) users.push_back(u);
    v.set("userIds", std::move(users));
    v.set("sortOrder", r.sort_order);
    v.set("active", r.active);
    return v;
}

AssignmentRule AutomationConfig::assignment_rule_from_json(const json::Value& v) {
    AssignmentRule r;
    r.id = v.get_string("id");
    r.name = v.get_string("name");
    r.object = v.get_string("object");
    r.criteria = v.get_string("criteria", "true");
    if (const json::Value* users = v.find("userIds"); users && users->is_array()) {
        for (const json::Value& u : users->as_array()) {
            if (u.is_string()) r.user_ids.push_back(u.as_string());
        }
    }
    r.sort_order = static_cast<int>(v.get_int("sortOrder"));
    r.active = v.get_bool("active", true);
    return r;
}

json::Value AutomationConfig::to_json() const {
    json::Value v = json::Value::object();
    json::Value vr = json::Value::array();
    for (const ValidationRule& r : validation_rules) vr.push_back(validation_rule_to_json(r));
    v.set("validationRules", std::move(vr));
    json::Value wr = json::Value::array();
    for (const WorkflowRule& r : workflow_rules) wr.push_back(workflow_rule_to_json(r));
    v.set("workflowRules", std::move(wr));
    json::Value ar = json::Value::array();
    for (const AssignmentRule& r : assignment_rules) ar.push_back(assignment_rule_to_json(r));
    v.set("assignmentRules", std::move(ar));
    return v;
}

AutomationConfig AutomationConfig::from_json(const json::Value& v) {
    AutomationConfig c;
    if (const json::Value* a = v.find("validationRules"); a && a->is_array()) {
        for (const json::Value& r : a->as_array()) c.validation_rules.push_back(validation_rule_from_json(r));
    }
    if (const json::Value* a = v.find("workflowRules"); a && a->is_array()) {
        for (const json::Value& r : a->as_array()) c.workflow_rules.push_back(workflow_rule_from_json(r));
    }
    if (const json::Value* a = v.find("assignmentRules"); a && a->is_array()) {
        for (const json::Value& r : a->as_array()) c.assignment_rules.push_back(assignment_rule_from_json(r));
    }
    return c;
}

// ---- OrgSettings ----------------------------------------------------------

json::Value OrgSettings::to_json() const {
    json::Value v = json::Value::object();
    v.set("timezone", timezone);
    v.set("currency", currency);
    v.set("locale", locale);
    json::Value rates = json::Value::object();
    for (const auto& [code, rate] : currency_rates) rates.set(code, rate);
    v.set("currencyRates", std::move(rates));
    v.set("apiCallsPerDay", api_calls_per_day);
    v.set("storageMb", storage_mb);
    v.set("maxQueryRows", max_query_rows);
    v.set("recycleBinDays", recycle_bin_days);
    v.set("duplicateRules", duplicate_rules);
    v.set("webForms", web_forms);
    json::Value bh = json::Value::object();
    bh.set("startMinute", hours.start_minute);
    bh.set("endMinute", hours.end_minute);
    bh.set("workdays", static_cast<int64_t>(hours.workdays));
    json::Value sla = json::Value::object();
    for (const auto& [prio, h] : hours.sla_hours) sla.set(prio, h);
    bh.set("slaHours", std::move(sla));
    v.set("businessHours", std::move(bh));
    return v;
}

OrgSettings OrgSettings::from_json(const json::Value& v) {
    OrgSettings s;
    s.timezone = v.get_string("timezone", s.timezone);
    s.currency = v.get_string("currency", s.currency);
    s.locale = v.get_string("locale", s.locale);
    if (const json::Value* rates = v.find("currencyRates"); rates && rates->is_object()) {
        for (const auto& [code, rate] : rates->as_object()) {
            if (rate.is_number()) s.currency_rates[code] = rate.as_double();
        }
    }
    s.api_calls_per_day = v.get_int("apiCallsPerDay", s.api_calls_per_day);
    s.storage_mb = v.get_int("storageMb", s.storage_mb);
    s.max_query_rows = v.get_int("maxQueryRows", s.max_query_rows);
    s.recycle_bin_days = static_cast<int>(v.get_int("recycleBinDays", s.recycle_bin_days));
    s.duplicate_rules = v.get_bool("duplicateRules", s.duplicate_rules);
    s.web_forms = v.get_bool("webForms", s.web_forms);
    if (const json::Value* bh = v.find("businessHours"); bh && bh->is_object()) {
        s.hours.start_minute = static_cast<int>(bh->get_int("startMinute", s.hours.start_minute));
        s.hours.end_minute = static_cast<int>(bh->get_int("endMinute", s.hours.end_minute));
        s.hours.workdays = static_cast<unsigned>(bh->get_int("workdays", s.hours.workdays));
        if (const json::Value* sla = bh->find("slaHours"); sla && sla->is_object()) {
            s.hours.sla_hours.clear();
            for (const auto& [prio, h] : sla->as_object()) {
                if (h.is_number()) s.hours.sla_hours[prio] = static_cast<int>(h.as_int());
            }
        }
    }
    return s;
}

// ---- Org ------------------------------------------------------------------

Org::Org(std::string id, std::string name, std::string pod, std::string dir)
    : db(std::move(dir)), ids(pod), id_(std::move(id)), name_(std::move(name)), pod_(std::move(pod)) {
    created_at_ = timeutil::now_ms();
}

void Org::set_active(bool on) {
    active_ = on;
    save_header();
}

void Org::set_name(const std::string& name) {
    name_ = name;
    save_header();
}

timeutil::TimeZone Org::tz() const {
    timeutil::TimeZone z;
    if (!timeutil::TimeZone::lookup(settings.timezone, &z)) z = timeutil::TimeZone::utc();
    return z;
}

int64_t Org::next_autonumber(const std::string& key) {
    const int64_t n = ++autonumbers[key];
    return n;
}

bool Org::open(std::string* error, bool* is_new) {
    if (!db.open(error)) return false;
    const json::Value* header = db.meta("org");
    *is_new = header == nullptr;
    schema = meta::Schema::standard();
    if (header == nullptr) return true;

    name_ = header->get_string("name", name_);
    created_at_ = header->get_int("createdAt", created_at_);
    active_ = header->get_bool("active", true);
    if (const json::Value* v = db.meta("schema")) schema.load_custom_json(*v);
    if (const json::Value* v = db.meta("users")) users.load_json(*v);
    if (const json::Value* v = db.meta("profiles")) profiles.load_json(*v);
    if (const json::Value* v = db.meta("roles")) roles.load_json(*v);
    sharing = security::SharingModel::standard();
    if (const json::Value* v = db.meta("sharing")) sharing = security::SharingModel::from_json(*v);
    if (const json::Value* v = db.meta("settings")) settings = OrgSettings::from_json(*v);
    if (const json::Value* v = db.meta("automation")) automation = AutomationConfig::from_json(*v);
    if (const json::Value* v = db.meta("counters"); v && v->is_object()) {
        for (const auto& [k, n] : v->as_object()) {
            if (n.is_number()) autonumbers[k] = n.as_int();
        }
    }

    // Make sure new ids sort after everything already stored.
    for (const std::string& t : db.table_names()) {
        for (const auto& [rid, r] : db.find_table(t)->rows()) ids.observe(rid);
    }
    for (const auto& u : users.all()) ids.observe(u.id);
    for (const auto& r : roles.all()) ids.observe(r.id);

    // Recreate indexes the schema asks for.
    for (const meta::ObjectDef* o : schema.objects()) {
        storage::Table& t = db.table(o->name);
        for (const meta::FieldDef& f : o->fields) {
            if (f.indexed || f.unique || f.external_id) t.ensure_index(f.name, f.unique || f.external_id);
        }
    }
    return true;
}

void Org::initialize(const std::string& admin_username, const std::string& admin_first, const std::string& admin_last,
                     const std::string& admin_email, const std::string& admin_password, uint32_t pbkdf2_iterations) {
    schema = meta::Schema::standard();
    for (security::Profile& p : security::default_profiles()) profiles.put(std::move(p));
    sharing = security::SharingModel::standard();

    security::Role ceo;
    ceo.id = new_id("00E");
    ceo.name = "CEO";
    roles.put(ceo);

    auth::User admin;
    admin.id = new_id("005");
    admin.username = admin_username;
    admin.first_name = admin_first;
    admin.last_name = admin_last;
    admin.email = admin_email;
    admin.title = "Administrator";
    admin.profile_id = profiles.find_by_name("System Administrator")->id;
    admin.role_id = ceo.id;
    admin.timezone = settings.timezone;
    admin.created_at = timeutil::now_ms();
    auth::set_password(&admin, admin_password, pbkdf2_iterations);
    users.add(std::move(admin));

    for (const meta::ObjectDef* o : schema.objects()) {
        storage::Table& t = db.table(o->name);
        for (const meta::FieldDef& f : o->fields) {
            if (f.indexed || f.unique || f.external_id) t.ensure_index(f.name, f.unique || f.external_id);
        }
    }

    save_header();
    save_schema();
    save_users();
    save_profiles();
    save_roles();
    save_sharing();
    save_settings();
    save_automation();
}

void Org::save_header() {
    json::Value h = json::Value::object();
    h.set("id", id_);
    h.set("name", name_);
    h.set("pod", pod_);
    h.set("createdAt", created_at_);
    h.set("active", active_);
    db.put_meta("org", h);
}

void Org::save_schema() {
    db.put_meta("schema", schema.custom_to_json());
}

void Org::save_users() {
    db.put_meta("users", users.to_json());
}

void Org::save_profiles() {
    db.put_meta("profiles", profiles.to_json());
}

void Org::save_roles() {
    db.put_meta("roles", roles.to_json());
}

void Org::save_sharing() {
    db.put_meta("sharing", sharing.to_json());
}

void Org::save_settings() {
    db.put_meta("settings", settings.to_json());
}

void Org::save_automation() {
    db.put_meta("automation", automation.to_json());
}

void Org::save_counters() {
    json::Value v = json::Value::object();
    for (const auto& [k, n] : autonumbers) v.set(k, n);
    db.put_meta("counters", v);
}

// ---- OrgRegistry ----------------------------------------------------------

OrgRegistry::OrgRegistry(std::string root, uint32_t pbkdf2_iterations)
    : root_(std::move(root)), iterations_(pbkdf2_iterations) {}

std::string OrgRegistry::org_dir(const std::string& id) const {
    if (root_.empty()) return {};
    return (fs::path(root_) / "orgs" / id).string();
}

bool OrgRegistry::load(std::string* error) {
    if (root_.empty()) return true;
    const fs::path dir = fs::path(root_) / "orgs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (error) *error = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    std::vector<std::string> ids_found;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory()) ids_found.push_back(entry.path().filename().string());
    }
    std::sort(ids_found.begin(), ids_found.end());
    for (const std::string& id : ids_found) {
        const std::string pod = id.size() >= 5 ? id.substr(3, 2) : "00";
        auto org = std::make_shared<Org>(id, "", pod, org_dir(id));
        bool is_new = false;
        std::string err;
        if (!org->open(&err, &is_new)) {
            log::error("registry", "skipping org ", id, ": ", err);
            continue;
        }
        if (is_new) {
            log::warn("registry", "skipping empty org directory ", id);
            continue;
        }
        std::lock_guard<std::mutex> lock(mu_);
        for (const auth::User& u : org->users.all()) usernames_[str::to_lower(u.username)] = id;
        org_ids_.observe(id);
        ++next_pod_;
        orgs_[id] = org;
    }
    log::info("registry", "loaded ", orgs_.size(), " orgs from ", root_);
    return true;
}

std::shared_ptr<Org> OrgRegistry::create(const NewOrg& spec) {
    const std::string name(str::trim(spec.name));
    if (name.empty()) throw errors::invalid_field("name", "Org name is required");
    if (spec.admin_username.empty() || spec.admin_username.find('@') == std::string::npos) {
        throw errors::invalid_field("adminUsername", "Admin username must look like an email address");
    }
    std::string reason;
    if (!auth::password_meets_policy(spec.admin_password, &reason)) throw errors::invalid_field("adminPassword", reason);
    if (username_taken(spec.admin_username)) {
        throw errors::bad_request("DUPLICATE_USERNAME", "Username " + spec.admin_username + " is taken");
    }

    std::string id;
    std::string pod;
    {
        std::lock_guard<std::mutex> lock(mu_);
        pod = ids::encode_base62(next_pod_++, 2);
        id = org_ids_.next("00D");
        id = id.substr(0, 3) + pod + id.substr(5);
        id = id.substr(0, 15) + ids::suffix_for(id.substr(0, 15));
    }
    auto org = std::make_shared<Org>(id, name, pod, org_dir(id));
    bool is_new = false;
    std::string err;
    if (!org->open(&err, &is_new)) throw errors::internal("cannot create org storage: " + err);
    if (!spec.timezone.empty()) {
        timeutil::TimeZone z;
        if (!timeutil::TimeZone::lookup(spec.timezone, &z)) throw errors::invalid_field("timezone", "Unknown time zone");
        org->settings.timezone = z.name();
    }
    if (!spec.currency.empty()) org->settings.currency = str::to_upper(spec.currency);
    org->initialize(spec.admin_username, spec.admin_first_name, spec.admin_last_name, spec.admin_email,
                    spec.admin_password, iterations_);
    std::lock_guard<std::mutex> lock(mu_);
    orgs_[id] = org;
    usernames_[str::to_lower(spec.admin_username)] = id;
    log::info("registry", "created org ", id, " (", name, ")");
    return org;
}

std::shared_ptr<Org> OrgRegistry::find(const std::string& id) const {
    std::string id18;
    if (!ids::normalize(id, &id18)) return nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = orgs_.find(id18);
    return it == orgs_.end() ? nullptr : it->second;
}

std::shared_ptr<Org> OrgRegistry::find_by_username(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = usernames_.find(str::to_lower(username));
    if (it == usernames_.end()) return nullptr;
    auto org = orgs_.find(it->second);
    return org == orgs_.end() ? nullptr : org->second;
}

std::vector<std::shared_ptr<Org>> OrgRegistry::all() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::shared_ptr<Org>> out;
    for (const auto& [id, org] : orgs_) out.push_back(org);
    return out;
}

void OrgRegistry::index_user(const std::string& username, const std::string& org_id) {
    std::lock_guard<std::mutex> lock(mu_);
    usernames_[str::to_lower(username)] = org_id;
}

void OrgRegistry::unindex_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(mu_);
    usernames_.erase(str::to_lower(username));
}

bool OrgRegistry::username_taken(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mu_);
    return usernames_.count(str::to_lower(username)) != 0;
}

void OrgRegistry::snapshot_if_needed(uint64_t threshold) {
    for (const auto& org : all()) {
        std::unique_lock<std::shared_mutex> lock(org->mutex());
        if (org->db.log_entries() < threshold) continue;
        std::string err;
        if (!org->db.snapshot(&err)) {
            log::error("registry", "snapshot of ", org->id(), " failed: ", err);
        } else {
            log::info("registry", "snapshot of ", org->id(), " written");
        }
    }
}

}  // namespace tcrm::engine
