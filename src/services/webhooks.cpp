// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/webhooks.hpp"

#include <algorithm>
#include <chrono>

#include "http/client.hpp"
#include "model/errors.hpp"
#include "util/crypto.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"
#include "util/url.hpp"

namespace tcrm::services {
namespace {

constexpr size_t kLogLimit = 500;
constexpr size_t kExcerptLimit = 2000;

}  // namespace

json::Value Webhook::to_json(bool include_secret) const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("url", url);
    if (include_secret) v.set("secret", secret);
    json::Value h = json::Value::object();
    for (const auto& [k, val] : headers) h.set(k, val);
    v.set("headers", std::move(h));
    v.set("active", active);
    v.set("createdDate", timeutil::format_datetime(created_at));
    return v;
}

Webhook Webhook::from_json(const json::Value& v) {
    Webhook w;
    w.id = v.get_string("id");
    w.name = v.get_string("name");
    w.url = v.get_string("url");
    w.secret = v.get_string("secret");
    if (const json::Value* h = v.find("headers"); h && h->is_object()) {
        for (const auto& [k, val] : h->as_object()) {
            if (val.is_string()) w.headers[k] = val.as_string();
        }
    }
    w.active = v.get_bool("active", true);
    int64_t created = 0;
    if (timeutil::parse_datetime(v.get_string("createdDate"), &created)) w.created_at = created;
    return w;
}

std::vector<Webhook> load_webhooks(const engine::Org& org) {
    std::vector<Webhook> out;
    const json::Value* v = org.db.meta("webhooks");
    if (v == nullptr || !v->is_array()) return out;
    for (const json::Value& w : v->as_array()) out.push_back(Webhook::from_json(w));
    return out;
}

void store_webhooks(engine::Org& org, const std::vector<Webhook>& hooks) {
    json::Value arr = json::Value::array();
    for (const Webhook& w : hooks) arr.push_back(w.to_json(true));
    org.db.put_meta("webhooks", arr);
}

void validate_webhook(const Webhook& w) {
    if (str::trim(w.name).empty()) throw errors::invalid_field("name", "Webhook name is required");
    url::Url u;
    if (!url::parse_url(w.url, &u)) throw errors::invalid_field("url", "Webhook URL must be an absolute http(s) URL");
    if (u.scheme != "http") {
        throw errors::invalid_field("url", "Only http:// endpoints are supported; route https through the egress relay");
    }
    for (const auto& [k, v] : w.headers) {
        if (k.empty() || str::iequals(k, "Content-Length") || str::iequals(k, "Host")) {
            throw errors::invalid_field("headers", "Header " + k + " can't be set");
        }
        (void)v;
    }
}

json::Value DeliveryRecord::to_json() const {
    json::Value v = json::Value::object();
    v.set("webhookId", webhook_id);
    v.set("url", url);
    v.set("attempt", attempt);
    v.set("status", status);
    if (!error.empty()) v.set("error", error);
    v.set("response", response_excerpt);
    v.set("at", timeutil::format_datetime(at));
    v.set("durationMs", duration_ms);
    return v;
}

WebhookService::WebhookService(size_t workers, int timeout_ms, int max_attempts)
    : worker_count_(workers == 0 ? 1 : workers), timeout_ms_(timeout_ms), max_attempts_(max_attempts) {}

WebhookService::~WebhookService() {
    stop();
}

void WebhookService::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!workers_.empty()) return;
    stopping_ = false;
    for (size_t i = 0; i < worker_count_; ++i) workers_.emplace_back([this] { run(); });
}

void WebhookService::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

std::string WebhookService::signature(const std::string& secret, const std::string& body) {
    return "sha256=" + crypto::to_hex(crypto::hmac_sha256(secret, body));
}

void WebhookService::webhook(engine::Org& org, const std::string& webhook_id, const json::Value& payload) {
    for (const Webhook& w : load_webhooks(org)) {
        if (w.id == webhook_id && w.active) {
            enqueue(org.id(), w, payload);
            return;
        }
    }
    log::warn("webhooks", org.id(), ": workflow refers to unknown or inactive webhook ", webhook_id);
}

void WebhookService::enqueue(const std::string& org_id, const Webhook& hook, const json::Value& payload) {
    Delivery d;
    d.org_id = org_id;
    d.hook = hook;
    json::Value envelope = json::Value::object();
    envelope.set("id", crypto::random_token(12));
    envelope.set("orgId", org_id);
    envelope.set("sentAt", timeutil::format_datetime(timeutil::now_ms()));
    envelope.set("data", payload);
    d.body = envelope.dump();
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(std::move(d));
    }
    cv_.notify_one();
}

std::vector<DeliveryRecord> WebhookService::recent(const std::string& org_id, size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<DeliveryRecord> out;
    for (auto it = log_.rbegin(); it != log_.rend() && out.size() < limit; ++it) {
        if (it->org_id == org_id) out.push_back(*it);
    }
    return out;
}

size_t WebhookService::pending() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

bool WebhookService::deliver(Delivery& d, DeliveryRecord* rec) {
    url::Url target;
    rec->org_id = d.org_id;
    rec->webhook_id = d.hook.id;
    rec->url = d.hook.url;
    rec->attempt = d.attempt + 1;
    rec->at = timeutil::now_ms();
    if (!url::parse_url(d.hook.url, &target)) {
        rec->error = "invalid URL";
        return true;  // permanent failure: don't retry
    }
    http::Headers headers;
    headers.set("Content-Type", "application/json");
    headers.set("X-TropiCRM-Delivery-Attempt", std::to_string(d.attempt + 1));
    if (!d.hook.secret.empty()) headers.set("X-TropiCRM-Signature", signature(d.hook.secret, d.body));
    for (const auto& [k, v] : d.hook.headers) headers.set(k, v);
    http::ClientOptions opts;
    opts.timeout_ms = timeout_ms_;
    http::ClientResponse resp;
    std::string err;
    const int64_t started = timeutil::now_ms();
    const bool ok = http::send_request(http::Method::Post, target, headers, d.body, opts, &resp, &err);
    rec->duration_ms = timeutil::now_ms() - started;
    if (!ok) {
        rec->error = err;
        return false;
    }
    rec->status = resp.status;
    rec->response_excerpt = str::ellipsize(resp.body, kExcerptLimit);
    return resp.status >= 200 && resp.status < 300;
}

void WebhookService::run() {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;
        const int64_t now = timeutil::now_ms();
        auto it = std::find_if(queue_.begin(), queue_.end(), [now](const Delivery& d) { return d.not_before <= now; });
        if (it == queue_.end()) continue;
        Delivery d = std::move(*it);
        queue_.erase(it);

        DeliveryRecord rec;
        const bool done = deliver(d, &rec);
        log_.push_back(rec);
        while (log_.size() > kLogLimit) log_.pop_front();
        if (done) continue;
        if (++d.attempt >= max_attempts_) {
            log::warn("webhooks", d.org_id, ": giving up on ", d.hook.url, " after ", d.attempt, " attempts");
            continue;
        }
        d.not_before = timeutil::now_ms() + (int64_t{1000} << std::min(d.attempt, 20));
        queue_.push_back(std::move(d));
    }
}

}  // namespace tcrm::services
