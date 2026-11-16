// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tcrm::services {

/// Per-user "recently viewed" lists, most recent first. In memory only.
class RecentItems {
public:
    explicit RecentItems(size_t per_user = 25) : per_user_(per_user) {}

    void touch(const std::string& org_id, const std::string& user_id, const std::string& record_id);
    void forget(const std::string& org_id, const std::string& record_id);

    /// Record ids, newest first.
    std::vector<std::string> list(const std::string& org_id, const std::string& user_id) const;

private:
    size_t per_user_;
    mutable std::mutex mu_;
    std::map<std::pair<std::string, std::string>, std::deque<std::string>> items_;
};

}  // namespace tcrm::services
