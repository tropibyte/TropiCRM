// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/outbox.hpp"

#include "util/log.hpp"
#include "util/time.hpp"

namespace tcrm::services {

json::Value OutgoingEmail::to_json() const {
    json::Value v = json::Value::object();
    v.set("toUserId", to_user_id);
    v.set("toAddress", to_address);
    v.set("subject", subject);
    v.set("body", body);
    v.set("queuedAt", timeutil::format_datetime(queued_at));
    return v;
}

void Outbox::email(engine::Org& org, const std::string& to_user_id, const std::string& subject,
                   const std::string& body) {
    const auth::User* u = org.users.find(to_user_id);
    send(org.id(), to_user_id, u ? u->email : std::string(), subject, body);
}

void Outbox::send(const std::string& org_id, const std::string& to_user_id, const std::string& to_address,
                  const std::string& subject, const std::string& body) {
    OutgoingEmail m;
    m.org_id = org_id;
    m.to_user_id = to_user_id;
    m.to_address = to_address;
    m.subject = subject;
    m.body = body;
    m.queued_at = timeutil::now_ms();
    log::info("outbox", org_id, ": to=", to_address.empty() ? to_user_id : to_address, " subject=\"", subject,
              "\" body=\"", body, "\"");
    std::lock_guard<std::mutex> lock(mu_);
    messages_.push_back(std::move(m));
    while (messages_.size() > capacity_) messages_.pop_front();
}

std::vector<OutgoingEmail> Outbox::recent(const std::string& org_id, size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<OutgoingEmail> out;
    for (auto it = messages_.rbegin(); it != messages_.rend() && out.size() < limit; ++it) {
        if (it->org_id == org_id) out.push_back(*it);
    }
    return out;
}

}  // namespace tcrm::services
