// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "engine/merge.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "engine/records.hpp"
#include "model/errors.hpp"
#include "util/id.hpp"
#include "util/log.hpp"
#include "util/time.hpp"

namespace tcrm::engine {

MergeResult merge_records(RecordService& records, const Principal& who, const MergeRequest& request) {
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    const meta::ObjectDef* obj = org.schema.object(request.object);
    if (obj == nullptr || (obj->name != "Account" && obj->name != "Contact" && obj->name != "Lead")) {
        throw errors::invalid_field("object", "Merge supports Account, Contact and Lead");
    }
    if (request.duplicate_ids.empty() || request.duplicate_ids.size() > 2) {
        throw errors::invalid_field("duplicateIds", "Merge one or two duplicates at a time");
    }
    AccessChecker access(org, who);
    access.require_object(*obj, ObjectOp::Delete);

    std::string master_id;
    if (!ids::normalize(request.master_id, &master_id)) throw errors::bad_request("MALFORMED_ID", "Malformed master id");
    storage::Table& table = org.db.table(obj->name);
    const Record* master = table.get(master_id);
    if (master == nullptr || master->is_deleted()) throw errors::not_found("No such master record");
    if (!access.can_edit(*obj, *master)) throw errors::forbidden();

    std::vector<std::string> dups;
    for (const std::string& raw : request.duplicate_ids) {
        std::string id;
        if (!ids::normalize(raw, &id)) throw errors::bad_request("MALFORMED_ID", "Malformed duplicate id " + raw);
        if (id == master_id) throw errors::invalid_field("duplicateIds", "The master can't be merged into itself");
        const Record* dup = table.get(id);
        if (dup == nullptr || dup->is_deleted()) throw errors::not_found("No such duplicate " + raw);
        dups.push_back(id);
    }

    // 1. Field values chosen for the master.
    MergeResult result;
    result.master_id = master_id;
    if (request.field_values.is_object() && request.field_values.size() > 0) {
        SaveOptions opts;
        opts.allow_duplicates = true;
        opts.run_workflows = false;
        records.update_locked(org, who, *obj, master_id, request.field_values, opts);
    }

    // 2. Re-point children of every object at the master.
    const int64_t now = timeutil::now_ms();
    for (const meta::ObjectDef* child_obj : org.schema.objects()) {
        if (child_obj->name == "User") continue;
        std::vector<const meta::FieldDef*> links;
        for (const meta::FieldDef& f : child_obj->fields) {
            if (!meta::is_reference_type(f.type) || f.system || f.name == "OwnerId") continue;
            if (f.reference_to == "*" || f.reference_to.find(obj->name) != std::string::npos) links.push_back(&f);
        }
        if (links.empty()) continue;
        storage::Table& ct = org.db.table(child_obj->name);
        std::vector<Record> changed;
        for (const auto& [cid, child] : ct.rows()) {
            bool touched = false;
            Record copy = child;
            for (const meta::FieldDef* link : links) {
                const std::string v = child.text(link->name);
                if (std::find(dups.begin(), dups.end(), v) != dups.end()) {
                    copy.set(link->name, FieldValue::string(master_id));
                    touched = true;
                }
            }
            if (touched) {
                copy.set(sysfield::kSystemModstamp, FieldValue::datetime(now));
                ++copy.version;
                changed.push_back(std::move(copy));
            }
        }
        for (Record& c : changed) {
            org.db.put(c);
            ++result.reparented;
        }
    }

    // 3. Duplicates go to the recycle bin.
    for (const std::string& id : dups) {
        Record dup = *table.get(id);
        dup.set(sysfield::kIsDeleted, FieldValue::boolean(true));
        dup.set(sysfield::kLastModifiedDate, FieldValue::datetime(now));
        dup.set(sysfield::kLastModifiedById, FieldValue::string(who.user_id));
        dup.set("_MergedInto", FieldValue::string(master_id));
        ++dup.version;
        org.db.put(dup);
        if (records.hooks()) records.hooks()->deleted(org, *obj, dup);
        result.merged_ids.push_back(id);
    }
    log::info("merge", org.id(), ": merged ", dups.size(), " ", obj->name, " records into ", master_id);
    return result;
}

TransferResult transfer_ownership(RecordService& records, const Principal& who, const std::string& object,
                                  const std::string& from_user, const std::string& to_user) {
    if (!who.profile.modify_all_data) throw errors::forbidden("Transferring records requires Modify All Data");
    Org& org = *who.org;
    std::unique_lock<std::shared_mutex> lock(org.mutex());
    const meta::ObjectDef* obj = org.schema.object(object);
    if (obj == nullptr || obj->field("OwnerId") == nullptr) throw errors::invalid_field("object", "Unsupported object");
    const auth::User* to = org.users.find(to_user);
    if (to == nullptr || !to->active) throw errors::invalid_field("toUserId", "No such active user");
    std::vector<std::string> ids_to_move;
    for (const auto& [id, r] : org.db.table(obj->name).rows()) {
        if (!r.is_deleted() && r.owner_id() == from_user) ids_to_move.push_back(id);
    }
    TransferResult result;
    json::Value update = json::Value::object();
    update.set("OwnerId", to_user);
    SaveOptions opts;
    opts.run_workflows = false;
    for (const std::string& id : ids_to_move) {
        try {
            records.update_locked(org, who, *obj, id, update, opts);
            ++result.transferred;
        } catch (const ApiError& e) {
            log::warn("transfer", org.id(), ": ", id, ": ", e.what());
            result.failed_ids.push_back(id);
        }
    }
    return result;
}

}  // namespace tcrm::engine
