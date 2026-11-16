// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "engine/record_service.hpp"
#include "json/json.hpp"

namespace tcrm::services {

struct OutgoingEmail {
    std::string org_id;
    std::string to_user_id;
    std::string to_address;
    std::string subject;
    std::string body;
    int64_t queued_at = 0;

    json::Value to_json() const;
};

/// Collects outgoing email. Deployments forward it with the relay job; in
/// development it is only logged.
class Outbox : public engine::RecordHooks {
public:
    explicit Outbox(size_t capacity = 1000) : capacity_(capacity) {}

    void email(engine::Org& org, const std::string& to_user_id, const std::string& subject,
               const std::string& body) override;

    /// For callers that know the address (password resets).
    void send(const std::string& org_id, const std::string& to_user_id, const std::string& to_address,
              const std::string& subject, const std::string& body);

    std::vector<OutgoingEmail> recent(const std::string& org_id, size_t limit) const;

private:
    size_t capacity_;
    mutable std::mutex mu_;
    std::deque<OutgoingEmail> messages_;
};

/// Forwards record events to several hook implementations.
class HookFanout : public engine::RecordHooks {
public:
    void add(engine::RecordHooks* h) { hooks_.push_back(h); }

    void saved(engine::Org& org, const meta::ObjectDef& obj, const Record* before, const Record& after) override {
        for (auto* h : hooks_) h->saved(org, obj, before, after);
    }
    void deleted(engine::Org& org, const meta::ObjectDef& obj, const Record& r) override {
        for (auto* h : hooks_) h->deleted(org, obj, r);
    }
    void undeleted(engine::Org& org, const meta::ObjectDef& obj, const Record& r) override {
        for (auto* h : hooks_) h->undeleted(org, obj, r);
    }
    void webhook(engine::Org& org, const std::string& webhook_id, const json::Value& payload) override {
        for (auto* h : hooks_) h->webhook(org, webhook_id, payload);
    }
    void email(engine::Org& org, const std::string& to_user_id, const std::string& subject,
               const std::string& body) override {
        for (auto* h : hooks_) h->email(org, to_user_id, subject, body);
    }

private:
    std::vector<engine::RecordHooks*> hooks_;
};

}  // namespace tcrm::services
