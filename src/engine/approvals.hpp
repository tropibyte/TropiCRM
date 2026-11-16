// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/record_service.hpp"
#include "json/json.hpp"

namespace tcrm::engine {

struct ApprovalStep {
    std::string name;
    std::string approver = "manager";  // "manager" (the submitter's manager) or a user id
};

/// "Records of <object> matching <entry_criteria> go through these steps;
/// on final approval set <field> = <value>."
struct ApprovalProcess {
    std::string id;
    std::string name;
    std::string object;
    std::string entry_criteria = "true";
    std::vector<ApprovalStep> steps;
    std::string approved_field;
    std::string approved_value;  // a formula
    std::string rejected_field;
    std::string rejected_value;  // a formula
    bool active = true;

    json::Value to_json() const;
    static ApprovalProcess from_json(const json::Value& v);
};

enum class ApprovalStatus { Pending, Approved, Rejected, Recalled };

const char* approval_status_name(ApprovalStatus s);

struct ApprovalAction {
    std::string actor_id;
    std::string action;  // Submitted, Approved, Rejected, Recalled
    std::string comments;
    int64_t at = 0;
};

struct ApprovalRequest {
    std::string id;
    std::string process_id;
    std::string object;
    std::string record_id;
    std::string submitter_id;
    std::string current_approver;
    size_t step = 0;
    ApprovalStatus status = ApprovalStatus::Pending;
    std::vector<ApprovalAction> history;
    int64_t created_at = 0;
    int64_t updated_at = 0;

    json::Value to_json() const;
    static ApprovalRequest from_json(const json::Value& v);
};

std::vector<ApprovalProcess> load_approval_processes(const Org& org);
void store_approval_processes(Org& org, const std::vector<ApprovalProcess>& processes);
std::vector<ApprovalRequest> load_approval_requests(const Org& org);
void store_approval_requests(Org& org, const std::vector<ApprovalRequest>& requests);

/// Validates a process definition against the schema. Throws ApiError.
void validate_approval_process(const Org& org, ApprovalProcess* p);

/// True when a pending approval request locks the record. The caller holds
/// the org lock.
bool record_locked(const Org& org, const std::string& record_id);

/// Approval workflow operations. Each takes the org lock itself.
class ApprovalService {
public:
    explicit ApprovalService(RecordService& records) : records_(records) {}

    /// Submits a record to the first active process whose entry criteria
    /// it meets.
    ApprovalRequest submit(const Principal& who, const std::string& record_id, const std::string& comments);

    /// action is "Approve", "Reject" or "Recall".
    ApprovalRequest act(const Principal& who, const std::string& request_id, const std::string& action,
                        const std::string& comments);

    /// Requests waiting on `who`, or submitted by them.
    std::vector<ApprovalRequest> pending_for(const Principal& who) const;
    std::vector<ApprovalRequest> submitted_by(const Principal& who) const;

private:
    static std::string resolve_approver(const Org& org, const ApprovalStep& step, const std::string& submitter);
    void apply_outcome(Org& org, const Principal& who, const ApprovalProcess& p, const ApprovalRequest& r,
                       bool approved);

    RecordService& records_;
};

}  // namespace tcrm::engine
