// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/access.hpp"
#include "engine/org.hpp"
#include "json/json.hpp"
#include "model/record.hpp"

namespace tcrm::engine {

/// Side effects of saves that live outside the engine: the search index,
/// webhooks and outgoing email. Called with the org lock held; must not
/// block.
class RecordHooks {
public:
    virtual ~RecordHooks() = default;
    virtual void saved(Org& org, const meta::ObjectDef& obj, const Record* before, const Record& after) {
        (void)org, (void)obj, (void)before, (void)after;
    }
    virtual void deleted(Org& org, const meta::ObjectDef& obj, const Record& r) { (void)org, (void)obj, (void)r; }
    virtual void undeleted(Org& org, const meta::ObjectDef& obj, const Record& r) { (void)org, (void)obj, (void)r; }
    virtual void webhook(Org& org, const std::string& webhook_id, const json::Value& payload) {
        (void)org, (void)webhook_id, (void)payload;
    }
    virtual void email(Org& org, const std::string& to_user_id, const std::string& subject, const std::string& body) {
        (void)org, (void)to_user_id, (void)subject, (void)body;
    }
};

struct SaveOptions {
    bool allow_duplicates = false;      // Sforce-Duplicate-Rule-Header: allowSave=true
    bool run_assignment_rules = true;
    bool run_workflows = true;
    uint64_t expected_version = 0;      // If-Match; 0 = don't check
};

/// One field-history entry.
struct HistoryEntry {
    std::string id;
    std::string record_id;
    std::string object;
    std::string field;
    json::Value old_value;
    json::Value new_value;
    std::string changed_by;
    int64_t changed_at = 0;
};

/// The create/update/delete pipeline: permissions, defaults, standard
/// object behavior, formulas, validation and duplicate rules, persistence,
/// history, roll-ups, workflow and hooks.
class RecordService {
public:
    explicit RecordService(RecordHooks* hooks = nullptr) : hooks_(hooks) {}

    // Public entry points; each takes the org lock itself.
    Record create(const Principal& who, const std::string& object, const json::Value& fields,
                  const SaveOptions& options = {});
    Record update(const Principal& who, const std::string& object, const std::string& id, const json::Value& fields,
                  const SaveOptions& options = {});

    struct UpsertResult {
        Record record;
        bool created = false;
    };
    UpsertResult upsert(const Principal& who, const std::string& object, const std::string& external_field,
                        const std::string& external_value, const json::Value& fields,
                        const SaveOptions& options = {});

    /// Moves a record (and cascade-delete children) to the recycle bin.
    void remove(const Principal& who, const std::string& object, const std::string& id);
    Record undelete(const Principal& who, const std::string& id);
    /// Permanently deletes a record that is in the recycle bin.
    void purge(const Principal& who, const std::string& id);

    /// Reads a record the principal can see. Deleted records are
    /// ENTITY_IS_DELETED.
    Record get(const Principal& who, const std::string& object, const std::string& id);

    std::vector<HistoryEntry> history(const Principal& who, const std::string& object, const std::string& id);

    /// Removes recycle-bin records older than the org's retention period.
    size_t purge_expired(Org& org, int64_t now_ms);

    // Building blocks for callers that already hold the org's unique lock
    // (lead conversion, workflow actions, bulk jobs).
    Record create_locked(Org& org, const Principal& who, const meta::ObjectDef& obj, const json::Value& fields,
                         const SaveOptions& options);
    Record update_locked(Org& org, const Principal& who, const meta::ObjectDef& obj, const std::string& id,
                         const json::Value& fields, const SaveOptions& options);
    void remove_locked(Org& org, const Principal& who, const meta::ObjectDef& obj, const std::string& id);

    RecordHooks* hooks() const { return hooks_; }

private:
    enum class Mode { Create, Update, Upsert };

    static const meta::ObjectDef& require_object(const Org& org, std::string_view name);

    void apply_fields(const Org& org, const AccessChecker& access, const meta::ObjectDef& obj,
                      const json::Value& fields, Mode mode, Record* draft) const;
    void check_references(const Org& org, const meta::ObjectDef& obj, const Record& r, const Record* before) const;
    void apply_defaults(const Org& org, const Principal& who, const meta::ObjectDef& obj, Record* r) const;
    void assign_autonumbers(Org& org, const meta::ObjectDef& obj, Record* r) const;
    void compute_formulas(const Org& org, const Principal& who, const meta::ObjectDef& obj, Record* r,
                          const Record* before) const;
    void check_required(const meta::ObjectDef& obj, const Record& r) const;
    void check_unique(const Org& org, const meta::ObjectDef& obj, const Record& r) const;
    void run_validation_rules(const Org& org, const Principal& who, const meta::ObjectDef& obj, const Record& r,
                              const Record* before) const;
    void check_duplicates(const Org& org, const meta::ObjectDef& obj, const Record& r) const;
    void run_assignment_rules(Org& org, const Principal& who, const meta::ObjectDef& obj, Record* r) const;
    void track_history(Org& org, const Principal& who, const meta::ObjectDef& obj, const Record& before,
                       const Record& after) const;
    void update_rollups(Org& org, const meta::ObjectDef& child_obj, const Record& child) const;
    void run_workflows(Org& org, const Principal& who, const meta::ObjectDef& obj, const Record& r,
                       const Record* before);

    Record save(Org& org, const Principal& who, const meta::ObjectDef& obj, Record draft, const Record* before,
                const SaveOptions& options);

    RecordHooks* hooks_;
};

/// Applies the built-in behavior of standard objects (opportunity stages,
/// case status, task completion, SLA dates...). Defined in behaviors.cpp.
void apply_standard_behavior(const Org& org, const meta::ObjectDef& obj, Record* r, const Record* before,
                             const json::Value& requested);

/// Adds `hours` of business time to `start_ms` using the org's business
/// hours.
int64_t add_business_hours(int64_t start_ms, int hours, const BusinessHours& bh);

/// Converts an amount from one currency to another using the org's rates.
double convert_currency(const OrgSettings& settings, double amount, const std::string& from, const std::string& to);

/// Default probability (0-100) and forecast category for an opportunity stage.
bool stage_defaults(const std::string& stage, int* probability, std::string* forecast, bool* closed, bool* won);

}  // namespace tcrm::engine
