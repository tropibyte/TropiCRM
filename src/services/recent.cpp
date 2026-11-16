// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/recent.hpp"

#include <algorithm>

namespace tcrm::services {

void RecentItems::touch(const std::string& org_id, const std::string& user_id, const std::string& record_id) {
    std::lock_guard<std::mutex> lock(mu_);
    std::deque<std::string>& q = items_[{org_id, user_id}];
    q.erase(std::remove(q.begin(), q.end(), record_id), q.end());
    q.push_front(record_id);
    while (q.size() > per_user_) q.pop_back();
}

void RecentItems::forget(const std::string& org_id, const std::string& record_id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [key, q] : items_) {
        if (key.first != org_id) continue;
        q.erase(std::remove(q.begin(), q.end(), record_id), q.end());
    }
}

std::vector<std::string> RecentItems::list(const std::string& org_id, const std::string& user_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = items_.find({org_id, user_id});
    if (it == items_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

}  // namespace tcrm::services
