// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "json/json.hpp"

namespace tcrm::services {

struct AuditEntry {
    std::string org_id;
    std::string user_id;
    std::string username;
    std::string section;  // e.g. "Users", "Sharing", "Customize"
    std::string action;
    std::string detail;
    int64_t at = 0;

    json::Value to_json() const;
};

/// The setup audit trail: who changed configuration, and when. Kept in
/// memory per org, newest last, bounded per org.
class AuditTrail {
public:
    explicit AuditTrail(size_t per_org_limit = 2000) : limit_(per_org_limit) {}

    void record(const std::string& org_id, const std::string& user_id, const std::string& username,
                const std::string& section, const std::string& action, const std::string& detail = {});

    /// Newest first.
    std::vector<AuditEntry> recent(const std::string& org_id, size_t limit, const std::string& section = {}) const;

private:
    size_t limit_;
    mutable std::mutex mu_;
    std::map<std::string, std::deque<AuditEntry>> entries_;
};

}  // namespace tcrm::services
