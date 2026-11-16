// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/record_service.hpp"
#include "json/json.hpp"

namespace tcrm::services {

struct Webhook {
    std::string id;
    std::string name;
    std::string url;
    std::string secret;  // HMAC key for the X-TropiCRM-Signature header
    std::map<std::string, std::string> headers;
    bool active = true;
    int64_t created_at = 0;

    json::Value to_json(bool include_secret) const;
    static Webhook from_json(const json::Value& v);
};

/// The org's webhook definitions, stored in the org's metadata. Callers
/// hold the org lock.
std::vector<Webhook> load_webhooks(const engine::Org& org);
void store_webhooks(engine::Org& org, const std::vector<Webhook>& hooks);

/// Validates a webhook definition from the API. Throws ApiError.
void validate_webhook(const Webhook& w);

struct DeliveryRecord {
    std::string org_id;
    std::string webhook_id;
    std::string url;
    int attempt = 0;
    int status = 0;
    std::string error;
    std::string response_excerpt;
    int64_t at = 0;
    int64_t duration_ms = 0;

    json::Value to_json() const;
};

/// Delivers webhook payloads on background threads, retrying failures with
/// exponential backoff.
class WebhookService : public engine::RecordHooks {
public:
    WebhookService(size_t workers, int timeout_ms, int max_attempts);
    ~WebhookService() override;

    void start();
    void stop();

    void webhook(engine::Org& org, const std::string& webhook_id, const json::Value& payload) override;

    /// Queues a delivery directly (used by the "test" endpoint).
    void enqueue(const std::string& org_id, const Webhook& hook, const json::Value& payload);

    std::vector<DeliveryRecord> recent(const std::string& org_id, size_t limit) const;
    size_t pending() const;

    static std::string signature(const std::string& secret, const std::string& body);

private:
    struct Delivery {
        std::string org_id;
        Webhook hook;
        std::string body;
        int attempt = 0;
        int64_t not_before = 0;
    };

    void run();
    bool deliver(Delivery& d, DeliveryRecord* rec);

    size_t worker_count_;
    int timeout_ms_;
    int max_attempts_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Delivery> queue_;
    std::deque<DeliveryRecord> log_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

}  // namespace tcrm::services
