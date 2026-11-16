// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "api/authenticator.hpp"
#include "auth/token.hpp"
#include "engine/approvals.hpp"
#include "engine/org.hpp"
#include "engine/record_service.hpp"
#include "http/router.hpp"
#include "json/json.hpp"
#include "model/errors.hpp"
#include "query/executor.hpp"
#include "services/audit.hpp"
#include "services/bulk.hpp"
#include "services/files.hpp"
#include "services/outbox.hpp"
#include "services/recent.hpp"
#include "services/search.hpp"
#include "services/webhooks.hpp"
#include "util/config.hpp"
#include "util/lru_cache.hpp"

namespace tcrm::api {

/// Request counters exported by /metrics.
struct Metrics {
    std::atomic<uint64_t> responses_2xx{0};
    std::atomic<uint64_t> responses_3xx{0};
    std::atomic<uint64_t> responses_4xx{0};
    std::atomic<uint64_t> responses_5xx{0};
    std::atomic<uint64_t> total_latency_ms{0};
};

/// Everything the route handlers need. Owned by the Application.
struct ApiContext {
    engine::OrgRegistry& registry;
    engine::RecordService& records;
    services::SearchService& search;
    services::WebhookService& webhooks;
    services::BulkService& bulk;
    services::FileStore& files;
    services::Outbox& outbox;
    query::CursorStore& cursors;
    auth::TokenService& tokens;
    Authenticator& auth;
    LruCache<std::string, json::Value>& describe_cache;
    const Config& config;
    http::Router& router;
    int64_t started_at = 0;
    engine::ApprovalService& approvals;
    services::AuditTrail& audit;
    Metrics& metrics;
    services::RecentItems& recent;
};

/// Records a setup change in the audit trail.
void audit(ApiContext& ctx, const engine::Principal& who, const std::string& section, const std::string& action,
           const std::string& detail = {});

// ---- helpers shared by the route files ------------------------------------

/// The authenticated principal attached by authed(). Throws 401 if absent.
const engine::Principal& principal(const http::Request& req);

/// Wraps a handler so it only runs for authenticated, API-enabled users in
/// active orgs, counting the call against the org's daily limit.
http::Handler authed(ApiContext& ctx, http::Handler handler);

http::Response json_response(int status, const json::Value& body);
http::Response error_response(const ApiError& e);

/// Parses the request body as a JSON object. Throws ApiError.
json::Value body_object(const http::Request& req);

/// Throws 403 unless `allowed`.
void require(bool allowed, const std::string& what);

std::string new_config_id(engine::Org& org, const std::string& prefix);

/// Describe output for one object, filtered by what the principal can see.
json::Value describe_object(const engine::Org& org, const engine::AccessChecker& access, const meta::ObjectDef& obj);

// ---- route registration ---------------------------------------------------

void register_auth_routes(http::Router& r, ApiContext& ctx);
void register_sobject_routes(http::Router& r, ApiContext& ctx);
void register_query_routes(http::Router& r, ApiContext& ctx);
void register_tooling_routes(http::Router& r, ApiContext& ctx);
void register_user_routes(http::Router& r, ApiContext& ctx);
void register_admin_routes(http::Router& r, ApiContext& ctx);
void register_report_routes(http::Router& r, ApiContext& ctx);
void register_bulk_routes(http::Router& r, ApiContext& ctx);
void register_webhook_routes(http::Router& r, ApiContext& ctx);
void register_misc_routes(http::Router& r, ApiContext& ctx);
void register_process_routes(http::Router& r, ApiContext& ctx);
void register_public_routes(http::Router& r, ApiContext& ctx);
void register_analytics_routes(http::Router& r, ApiContext& ctx);
void register_setup_routes(http::Router& r, ApiContext& ctx);

}  // namespace tcrm::api
