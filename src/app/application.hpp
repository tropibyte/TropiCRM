// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "api/authenticator.hpp"
#include "api/context.hpp"
#include "api/rate_limiter.hpp"
#include "auth/token.hpp"
#include "engine/approvals.hpp"
#include "engine/org.hpp"
#include "engine/record_service.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "query/executor.hpp"
#include "services/audit.hpp"
#include "services/bulk.hpp"
#include "services/files.hpp"
#include "services/outbox.hpp"
#include "services/search.hpp"
#include "services/webhooks.hpp"
#include "util/config.hpp"
#include "util/lru_cache.hpp"

namespace tcrm {

/// The whole server: storage, services, the REST API and the HTTP
/// listener. Tests drive it in-process through handle().
class Application {
public:
    explicit Application(Config config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Loads orgs and starts background services. Must succeed before
    /// handle() or listen().
    bool init(std::string* error);

    /// Runs one request through middleware and the router.
    http::Response handle(http::Request& req);

    /// Starts the HTTP listener (non-blocking).
    bool listen(std::string* error);
    int port() const;

    /// Stops the listener and background work.
    void shutdown();

    engine::OrgRegistry& registry() { return *registry_; }
    engine::RecordService& records() { return *records_; }
    services::SearchService& search() { return search_; }
    services::Outbox& outbox() { return outbox_; }
    services::AuditTrail& audit() { return audit_; }
    services::BulkService& bulk() { return *bulk_; }
    const Config& config() const { return config_; }
    const http::Router& router() const { return router_; }

    /// One pass of periodic maintenance (snapshots, recycle-bin purge).
    void run_maintenance();

private:
    std::string load_or_create_signing_key(std::string* error);
    void add_cors_headers(const http::Request& req, http::Response* resp) const;
    void log_request(const http::Request& req, const http::Response& resp, int64_t elapsed_ms) const;
    void maintenance_loop();

    Config config_;
    std::unique_ptr<engine::OrgRegistry> registry_;
    services::SearchService search_;
    services::Outbox outbox_;
    std::unique_ptr<services::WebhookService> webhooks_;
    services::HookFanout hooks_;
    std::unique_ptr<engine::RecordService> records_;
    std::unique_ptr<engine::ApprovalService> approvals_;
    services::AuditTrail audit_;
    api::Metrics metrics_;
    services::RecentItems recent_;
    std::unique_ptr<services::BulkService> bulk_;
    std::unique_ptr<services::FileStore> files_;
    query::CursorStore cursors_;
    std::unique_ptr<auth::TokenService> tokens_;
    std::unique_ptr<api::Authenticator> auth_;
    LruCache<std::string, json::Value> describe_cache_{512};
    std::unique_ptr<api::RateLimiter> public_limiter_;
    http::Router router_;
    std::unique_ptr<api::ApiContext> ctx_;
    std::unique_ptr<http::Server> server_;

    std::thread maintenance_;
    std::mutex maintenance_mu_;
    std::condition_variable maintenance_cv_;
    bool stopping_ = false;
    bool initialized_ = false;
};

}  // namespace tcrm
