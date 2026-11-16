// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/record_service.hpp"
#include "json/json.hpp"

namespace tcrm::services {

enum class JobState { Open, UploadComplete, InProgress, JobComplete, Failed, Aborted };

const char* job_state_name(JobState s);

/// A Bulk API ingest job: CSV rows inserted, updated, upserted or deleted
/// in the background.
struct BulkJob {
    std::string id;
    std::string org_id;
    std::string object;
    std::string operation;  // insert, update, upsert, delete
    std::string external_id_field;
    std::string created_by;
    int64_t created_at = 0;
    int64_t completed_at = 0;
    std::atomic<JobState> state{JobState::Open};
    std::string data;  // uploaded CSV
    int64_t processed = 0;
    int64_t failed = 0;
    std::string error;
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> success_rows;  // sf__Id, sf__Created, columns...
    std::vector<std::vector<std::string>> failed_rows;   // sf__Id, sf__Error, columns...
    engine::Principal owner;

    json::Value info() const;
};

class BulkService {
public:
    BulkService(engine::RecordService& records, size_t workers);
    ~BulkService();

    void start();
    void stop();

    /// Throws ApiError for unsupported objects or operations.
    std::string create(const engine::Principal& who, const std::string& object, const std::string& operation,
                       const std::string& external_id_field);

    /// Appends CSV data to an open job.
    void upload(const engine::Principal& who, const std::string& job_id, const std::string& csv);

    /// Moves a job to UploadComplete (queues it) or Aborted.
    json::Value set_state(const engine::Principal& who, const std::string& job_id, const std::string& state);

    json::Value info(const engine::Principal& who, const std::string& job_id) const;
    json::Value list(const engine::Principal& who) const;
    std::string results_csv(const engine::Principal& who, const std::string& job_id, bool failed) const;

    void remove(const engine::Principal& who, const std::string& job_id);

private:
    BulkJob* find_owned(const engine::Principal& who, const std::string& job_id) const;
    void run();
    void process(BulkJob* job);

    engine::RecordService& records_;
    size_t worker_count_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, std::unique_ptr<BulkJob>> jobs_;
    std::deque<std::string> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    uint64_t next_id_ = 1;
};

}  // namespace tcrm::services
