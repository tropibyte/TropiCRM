// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <vector>

#include "engine/record_service.hpp"
#include "json/json.hpp"

namespace tcrm::engine {

struct MergeRequest {
    std::string object;  // Account, Contact or Lead
    std::string master_id;
    std::vector<std::string> duplicate_ids;  // at most two
    json::Value field_values;                // values to keep on the master, e.g. a duplicate's Phone
};

struct MergeResult {
    std::string master_id;
    std::vector<std::string> merged_ids;
    size_t reparented = 0;  // child records moved to the master
};

/// Merges duplicates into a master record: applies the chosen field values
/// to the master, re-points every child record at the master, and moves
/// the duplicates to the recycle bin.
MergeResult merge_records(RecordService& records, const Principal& who, const MergeRequest& request);

struct TransferResult {
    size_t transferred = 0;
    std::vector<std::string> failed_ids;
};

/// Reassigns every record of `object` owned by `from_user` to `to_user`.
TransferResult transfer_ownership(RecordService& records, const Principal& who, const std::string& object,
                                  const std::string& from_user, const std::string& to_user);

}  // namespace tcrm::engine
