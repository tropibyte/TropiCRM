// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/bulk.hpp"

#include <shared_mutex>

#include "meta/codec.hpp"
#include "model/errors.hpp"
#include "util/csv.hpp"
#include "util/id.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::services {
namespace {

constexpr size_t kMaxJobBytes = 150 * 1024 * 1024;

bool parse_state(const std::string& s, JobState* out) {
    if (str::iequals(s, "UploadComplete")) *out = JobState::UploadComplete;
    else if (str::iequals(s, "Aborted")) *out = JobState::Aborted;
    else return false;
    return true;
}

}  // namespace

const char* job_state_name(JobState s) {
    switch (s) {
        case JobState::Open: return "Open";
        case JobState::UploadComplete: return "UploadComplete";
        case JobState::InProgress: return "InProgress";
        case JobState::JobComplete: return "JobComplete";
        case JobState::Failed: return "Failed";
        case JobState::Aborted: return "Aborted";
    }
    return "Open";
}

json::Value BulkJob::info() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("object", object);
    v.set("operation", operation);
    if (!external_id_field.empty()) v.set("externalIdFieldName", external_id_field);
    v.set("state", job_state_name(state.load()));
    v.set("createdById", created_by);
    v.set("createdDate", timeutil::format_datetime(created_at));
    if (completed_at != 0) v.set("systemModstamp", timeutil::format_datetime(completed_at));
    v.set("numberRecordsProcessed", processed);
    v.set("numberRecordsFailed", failed);
    v.set("contentType", "CSV");
    v.set("apiVersion", "1.0");
    if (!error.empty()) v.set("errorMessage", error);
    return v;
}

BulkService::BulkService(engine::RecordService& records, size_t workers)
    : records_(records), worker_count_(workers == 0 ? 1 : workers) {}

BulkService::~BulkService() {
    stop();
}

void BulkService::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!workers_.empty()) return;
    stopping_ = false;
    for (size_t i = 0; i < worker_count_; ++i) workers_.emplace_back([this] { run(); });
}

void BulkService::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

std::string BulkService::create(const engine::Principal& who, const std::string& object, const std::string& operation,
                                const std::string& external_id_field) {
    if (!who.profile.bulk_api && !who.profile.modify_all_data) {
        throw errors::forbidden("The Bulk API is not enabled for your profile");
    }
    const std::string op = str::to_lower(operation);
    if (op != "insert" && op != "update" && op != "upsert" && op != "delete") {
        throw errors::invalid_field("operation", "operation must be insert, update, upsert or delete");
    }
    std::string canonical;
    {
        std::shared_lock<std::shared_mutex> lock(who.org->mutex());
        const meta::ObjectDef* obj = who.org->schema.object(object);
        if (obj == nullptr || obj->name == "User") throw errors::invalid_field("object", "Unsupported object " + object);
        canonical = obj->name;
        if (op == "upsert") {
            const meta::FieldDef* f = obj->field(external_id_field);
            if (f == nullptr || (!f->external_id && f->name != "Id")) {
                throw errors::invalid_field("externalIdFieldName", "Upsert needs an external id field");
            }
        }
    }
    auto job = std::make_unique<BulkJob>();
    std::lock_guard<std::mutex> lock(mu_);
    job->id = "750" + who.org->pod() + ids::encode_base62(next_id_++, 10);
    job->id += ids::suffix_for(job->id);
    job->org_id = who.org->id();
    job->object = canonical;
    job->operation = op;
    job->external_id_field = external_id_field;
    job->created_by = who.user_id;
    job->created_at = timeutil::now_ms();
    job->owner = who;
    const std::string id = job->id;
    jobs_[id] = std::move(job);
    return id;
}

BulkJob* BulkService::find_owned(const engine::Principal& who, const std::string& job_id) const {
    auto it = jobs_.find(job_id);
    if (it == jobs_.end() || it->second->org_id != who.org->id()) throw errors::not_found("No such job: " + job_id);
    BulkJob* job = it->second.get();
    if (job->created_by != who.user_id && !who.profile.modify_all_data) throw errors::not_found("No such job: " + job_id);
    return job;
}

void BulkService::upload(const engine::Principal& who, const std::string& job_id, const std::string& csv) {
    std::lock_guard<std::mutex> lock(mu_);
    BulkJob* job = find_owned(who, job_id);
    if (job->state != JobState::Open) throw errors::bad_request("INVALIDJOBSTATE", "Data can only be added to open jobs");
    if (job->data.size() + csv.size() > kMaxJobBytes) throw errors::bad_request("EXCEEDED_QUOTA", "Job data is too large");
    if (!job->data.empty() && !csv.empty()) {
        // Later uploads repeat the header row; drop it.
        const size_t eol = csv.find('\n');
        job->data += eol == std::string::npos ? std::string() : csv.substr(eol + 1);
    } else {
        job->data += csv;
    }
}

json::Value BulkService::set_state(const engine::Principal& who, const std::string& job_id, const std::string& state) {
    JobState target;
    if (!parse_state(state, &target)) throw errors::invalid_field("state", "state must be UploadComplete or Aborted");
    {
        std::lock_guard<std::mutex> lock(mu_);
        BulkJob* job = find_owned(who, job_id);
        const JobState cur = job->state;
        if (target == JobState::UploadComplete) {
            if (cur != JobState::Open) throw errors::bad_request("INVALIDJOBSTATE", "Job is not open");
            job->state = JobState::UploadComplete;
            queue_.push_back(job_id);
        } else {
            if (cur == JobState::JobComplete || cur == JobState::Failed) {
                throw errors::bad_request("INVALIDJOBSTATE", "Job has already finished");
            }
            job->state = JobState::Aborted;
        }
    }
    cv_.notify_one();
    return info(who, job_id);
}

json::Value BulkService::info(const engine::Principal& who, const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return find_owned(who, job_id)->info();
}

json::Value BulkService::list(const engine::Principal& who) const {
    std::lock_guard<std::mutex> lock(mu_);
    json::Value records = json::Value::array();
    for (const auto& [id, job] : jobs_) {
        if (job->org_id != who.org->id()) continue;
        if (job->created_by != who.user_id && !who.profile.modify_all_data) continue;
        records.push_back(job->info());
    }
    json::Value out = json::Value::object();
    out.set("done", true);
    out.set("records", std::move(records));
    return out;
}

std::string BulkService::results_csv(const engine::Principal& who, const std::string& job_id, bool failed) const {
    std::lock_guard<std::mutex> lock(mu_);
    const BulkJob* job = find_owned(who, job_id);
    if (job->state != JobState::JobComplete && job->state != JobState::Failed) {
        throw errors::bad_request("INVALIDJOBSTATE", "Results are available once the job completes");
    }
    std::vector<std::string> header;
    header.push_back("sf__Id");
    header.push_back(failed ? "sf__Error" : "sf__Created");
    for (const std::string& h : job->headers) header.push_back(h);
    std::string out = csv::format_row(header);
    for (const auto& row : failed ? job->failed_rows : job->success_rows) out += csv::format_row(row);
    return out;
}

void BulkService::remove(const engine::Principal& who, const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mu_);
    BulkJob* job = find_owned(who, job_id);
    const JobState s = job->state;
    if (s == JobState::Open || s == JobState::UploadComplete) {
        throw errors::bad_request("INVALIDJOBSTATE", "Abort the job before deleting it");
    }
    jobs_.erase(job_id);
}

void BulkService::run() {
    while (true) {
        BulkJob* job = nullptr;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            const std::string id = queue_.front();
            queue_.pop_front();
            auto it = jobs_.find(id);
            if (it == jobs_.end() || it->second->state != JobState::UploadComplete) continue;
            job = it->second.get();
            job->state = JobState::InProgress;
        }
        try {
            process(job);
        } catch (const std::exception& e) {
            job->error = e.what();
            job->state = JobState::Failed;
        }
    }
}

void BulkService::process(BulkJob* job) {
    csv::Reader reader(job->data);
    std::vector<std::string> header;
    if (!reader.next(&header)) {
        job->error = reader.error().empty() ? "The job has no data" : reader.error();
        job->state = JobState::Failed;
        return;
    }
    // Map columns to fields.
    std::vector<const meta::FieldDef*> fields;
    {
        std::shared_lock<std::shared_mutex> lock(job->owner.org->mutex());
        const meta::ObjectDef* obj = job->owner.org->schema.object(job->object);
        for (const std::string& h : header) {
            const meta::FieldDef* f = obj->field(str::trim(h));
            if (f == nullptr) {
                job->error = "Field name not found: " + h;
                job->state = JobState::Failed;
                return;
            }
            fields.push_back(f);
        }
    }
    job->headers = header;

    std::vector<std::string> row;
    while (reader.next(&row)) {
        if (job->state == JobState::Aborted) break;
        if (row.size() == 1 && row[0].empty()) continue;
        std::string id;
        bool created = false;
        try {
            if (row.size() != header.size()) {
                throw errors::bad_request("INVALID_ROW", "Row has " + std::to_string(row.size()) +
                                                             " columns; the header has " + std::to_string(header.size()));
            }
            json::Value values = json::Value::object();
            std::string ext_value;
            for (size_t i = 0; i < row.size(); ++i) {
                const meta::FieldDef* f = fields[i];
                if (f->name == "Id") {
                    id = row[i];
                    continue;
                }
                if (job->operation == "upsert" && str::iequals(f->name, job->external_id_field)) {
                    ext_value = row[i];
                    continue;
                }
                const FieldValue v = meta::value_from_text(*f, row[i]);
                switch (v.kind()) {
                    case ValueKind::Null: values.set(f->name, json::Value()); break;
                    case ValueKind::Bool: values.set(f->name, v.as_bool()); break;
                    case ValueKind::Int:
                    case ValueKind::Double: values.set(f->name, v.as_double()); break;
                    default: values.set(f->name, v.to_text()); break;
                }
            }
            if (job->operation == "insert") {
                id = records_.create(job->owner, job->object, values).id;
                created = true;
            } else if (job->operation == "update") {
                if (id.empty()) throw errors::bad_request("MISSING_ARGUMENT", "Id not specified in an update call");
                records_.update(job->owner, job->object, id, values);
            } else if (job->operation == "upsert") {
                auto r = records_.upsert(job->owner, job->object, job->external_id_field, ext_value, values);
                id = r.record.id;
                created = r.created;
            } else {
                if (id.empty()) throw errors::bad_request("MISSING_ARGUMENT", "Id not specified in a delete call");
                records_.remove(job->owner, job->object, id);
            }
            std::vector<std::string> out{id, created ? "true" : "false"};
            out.insert(out.end(), row.begin(), row.end());
            job->success_rows.push_back(std::move(out));
        } catch (const ApiError& e) {
            ++job->failed;
            std::vector<std::string> out{id, e.code() + ":" + e.what()};
            out.insert(out.end(), row.begin(), row.end());
            job->failed_rows.push_back(std::move(out));
        }
        ++job->processed;
    }
    if (!reader.error().empty()) job->error = reader.error();
    job->completed_at = timeutil::now_ms();
    if (job->state != JobState::Aborted) job->state = JobState::JobComplete;
    log::info("bulk", job->org_id, ": job ", job->id, " processed ", job->processed, " rows, ", job->failed, " failed");
}

}  // namespace tcrm::services
