// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/audit.hpp"

#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::services {

json::Value AuditEntry::to_json() const {
    json::Value v = json::Value::object();
    v.set("createdDate", timeutil::format_datetime(at));
    v.set("createdById", user_id);
    v.set("createdBy", username);
    v.set("section", section);
    v.set("action", action);
    v.set("display", detail);
    return v;
}

void AuditTrail::record(const std::string& org_id, const std::string& user_id, const std::string& username,
                        const std::string& section, const std::string& action, const std::string& detail) {
    AuditEntry e;
    e.org_id = org_id;
    e.user_id = user_id;
    e.username = username;
    e.section = section;
    e.action = action;
    e.detail = detail;
    e.at = timeutil::now_ms();
    std::lock_guard<std::mutex> lock(mu_);
    std::deque<AuditEntry>& q = entries_[org_id];
    q.push_back(std::move(e));
    while (q.size() > limit_) q.pop_front();
}

std::vector<AuditEntry> AuditTrail::recent(const std::string& org_id, size_t limit, const std::string& section) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<AuditEntry> out;
    auto it = entries_.find(org_id);
    if (it == entries_.end()) return out;
    for (auto e = it->second.rbegin(); e != it->second.rend() && out.size() < limit; ++e) {
        if (!section.empty() && !str::iequals(e->section, section)) continue;
        out.push_back(*e);
    }
    return out;
}

}  // namespace tcrm::services
