// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "app/application.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "util/crypto.hpp"
#include "util/log.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace fs = std::filesystem;

namespace tcrm {

Application::Application(Config config) : config_(std::move(config)) {}

Application::~Application() {
    shutdown();
}

std::string Application::load_or_create_signing_key(std::string* error) {
    const std::string dir = config_.get("data.dir");
    if (dir.empty()) return crypto::random_bytes(32);
    const fs::path path = fs::path(dir) / "signing.key";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (fs::exists(path)) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string key;
        if (!crypto::from_hex(str::trim(ss.str()), &key) || key.size() < 32) {
            if (error) *error = "corrupt signing key in " + path.string();
            return {};
        }
        return key;
    }
    const std::string key = crypto::random_bytes(32);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << crypto::to_hex(key) << "\n";
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return {};
    }
    log::info("app", "generated a new token signing key in ", path.string());
    return key;
}

bool Application::init(std::string* error) {
    if (initialized_) return true;
    const std::string data_dir = config_.get("data.dir");
    const auto iterations = static_cast<uint32_t>(config_.get_int("auth.pbkdf2_iterations", 100000));
    registry_ = std::make_unique<engine::OrgRegistry>(data_dir, iterations);
    if (!registry_->load(error)) return false;

    const std::string key = load_or_create_signing_key(error);
    if (key.empty()) return false;
    tokens_ = std::make_unique<auth::TokenService>(key, config_.get_int("auth.token_ttl_seconds", 7200));

    webhooks_ = std::make_unique<services::WebhookService>(
        static_cast<size_t>(config_.get_int("webhooks.workers", 2)),
        static_cast<int>(config_.get_int("webhooks.timeout_ms", 5000)),
        static_cast<int>(config_.get_int("webhooks.max_attempts", 10)));
    hooks_.add(&search_);
    hooks_.add(webhooks_.get());
    hooks_.add(&outbox_);
    records_ = std::make_unique<engine::RecordService>(&hooks_);
    approvals_ = std::make_unique<engine::ApprovalService>(*records_);
    bulk_ = std::make_unique<services::BulkService>(*records_, 2);
    files_ = std::make_unique<services::FileStore>(data_dir);

    api::AuthSettings auth_settings;
    auth_settings.max_failed_logins = static_cast<int>(config_.get_int("auth.max_failed_logins", 5));
    auth_settings.lockout_minutes = static_cast<int>(config_.get_int("auth.lockout_minutes", 15));
    auth_settings.reset_ttl_minutes = static_cast<int>(config_.get_int("auth.password_reset_ttl_minutes", 60));
    auth_settings.refresh_ttl_days = static_cast<int>(config_.get_int("auth.refresh_ttl_days", 30));
    auth_settings.pbkdf2_iterations = iterations;
    auth_settings.platform_admin_key_sha256 = config_.get("platform.admin_key_sha256");
    auth_ = std::make_unique<api::Authenticator>(*registry_, *tokens_, outbox_, auth_settings);
    const double per_minute = static_cast<double>(config_.get_int("limits.public_requests_per_minute", 30));
    public_limiter_ = std::make_unique<api::RateLimiter>(per_minute / 2, per_minute);

    ctx_ = std::unique_ptr<api::ApiContext>(new api::ApiContext{*registry_, *records_, search_, *webhooks_, *bulk_,
                                                                *files_, outbox_, cursors_, *tokens_, *auth_,
                                                                describe_cache_, config_, router_,
                                                                timeutil::now_ms(), *approvals_, audit_,
                                                                metrics_, recent_});
    api::register_auth_routes(router_, *ctx_);
    api::register_sobject_routes(router_, *ctx_);
    api::register_query_routes(router_, *ctx_);
    api::register_tooling_routes(router_, *ctx_);
    api::register_user_routes(router_, *ctx_);
    api::register_admin_routes(router_, *ctx_);
    api::register_report_routes(router_, *ctx_);
    api::register_bulk_routes(router_, *ctx_);
    api::register_webhook_routes(router_, *ctx_);
    api::register_misc_routes(router_, *ctx_);
    api::register_process_routes(router_, *ctx_);
    api::register_public_routes(router_, *ctx_);
    api::register_analytics_routes(router_, *ctx_);
    api::register_setup_routes(router_, *ctx_);

    for (const auto& org : registry_->all()) search_.rebuild(*org);
    webhooks_->start();
    bulk_->start();
    maintenance_ = std::thread([this] { maintenance_loop(); });
    initialized_ = true;
    log::info("app", "ready: ", registry_->all().size(), " orgs, ", router_.route_count(), " routes");
    return true;
}

void Application::add_cors_headers(const http::Request& req, http::Response* resp) const {
    const std::string origin = req.headers.get("Origin");
    if (origin.empty()) return;
    const std::vector<std::string> allowed = str::split_trimmed(config_.get("cors.allowed_origins"), ',');
    bool ok = allowed.empty();
    for (const std::string& a : allowed) {
        if (a == "*" || str::iequals(a, origin)) ok = true;
    }
    if (!ok) return;
    resp->headers.set("Access-Control-Allow-Origin", origin);
    resp->headers.set("Access-Control-Allow-Credentials", "true");
    resp->headers.set("Vary", "Origin");
}

void Application::log_request(const http::Request& req, const http::Response& resp, int64_t elapsed_ms) const {
    if (!config_.get_bool("log.requests", true)) return;
    std::string who = "-";
    if (const auto* p = std::any_cast<engine::Principal>(&req.auth)) who = p->username;
    log::info("http", req.request_id, " ", api::client_ip(req), " ", who, " ", req.method_text, " ", req.target, " ",
              resp.status, " ", elapsed_ms, "ms");
    if (resp.status >= 400 && !req.body.empty() && req.body.size() < 4096) {
        log::info("http", req.request_id, " request body: ", req.body);
    }
}

http::Response Application::handle(http::Request& req) {
    const int64_t started = timeutil::now_ms();
    http::Response resp;
    if (req.method == http::Method::Options && req.headers.has("Origin")) {
        resp.status = 204;
        resp.headers.set("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
        resp.headers.set("Access-Control-Allow-Headers",
                         "Authorization, Content-Type, If-Match, If-None-Match, X-File-Name, Sforce-Query-Options, "
                         "Sforce-Duplicate-Rule-Header, Sforce-Auto-Assign");
        resp.headers.set("Access-Control-Max-Age", "600");
        add_cors_headers(req, &resp);
        return resp;
    }
    // Unauthenticated endpoints (logins, resets, signups, web forms) are
    // throttled per client.
    if (str::istarts_with(req.path, "/services/oauth2/") || str::istarts_with(req.path, "/services/auth/") ||
        str::istarts_with(req.path, "/services/web2") || str::istarts_with(req.path, "/services/signup")) {
        int64_t retry_ms = 0;
        if (!public_limiter_->allow(api::client_ip(req), started, &retry_ms)) {
            resp = api::error_response(errors::limit_exceeded("Too many requests; slow down"));
            resp.headers.set("Retry-After", std::to_string(retry_ms / 1000 + 1));
            add_cors_headers(req, &resp);
            ++metrics_.responses_4xx;
            log_request(req, resp, timeutil::now_ms() - started);
            return resp;
        }
    }
    try {
        http::Router::Match m = router_.match(req);
        if (m.handler == nullptr) {
            if (!m.allowed.empty()) {
                std::vector<std::string> names;
                for (http::Method a : m.allowed) names.emplace_back(http::method_name(a));
                resp = api::error_response(ApiError(405, "METHOD_NOT_ALLOWED", "HTTP Method '" + req.method_text +
                                                                                   "' not allowed. Allowed are " +
                                                                                   str::join(names, ",")));
                resp.headers.set("Allow", str::join(names, ", "));
            } else {
                resp = api::error_response(errors::not_found("The requested resource does not exist"));
            }
        } else {
            resp = (*m.handler)(req);
        }
    } catch (const ApiError& e) {
        resp = api::error_response(e);
    } catch (const json::ParseError& e) {
        resp = api::error_response(errors::bad_request("JSON_PARSER_ERROR", e.what()));
    } catch (const json::TypeError& e) {
        resp = api::error_response(errors::bad_request("JSON_PARSER_ERROR", e.what()));
    } catch (const std::exception& e) {
        log::error("app", req.request_id, " ", req.method_text, " ", req.path, ": ", e.what());
        resp = api::error_response(errors::internal(std::string("An unexpected error occurred: ") + e.what()));
    }
    add_cors_headers(req, &resp);
    const int64_t elapsed = timeutil::now_ms() - started;
    if (resp.status >= 500) ++metrics_.responses_5xx;
    else if (resp.status >= 400) ++metrics_.responses_4xx;
    else if (resp.status >= 300) ++metrics_.responses_3xx;
    else ++metrics_.responses_2xx;
    metrics_.total_latency_ms += static_cast<uint64_t>(elapsed);
    log_request(req, resp, elapsed);
    return resp;
}

bool Application::listen(std::string* error) {
    if (!initialized_ && !init(error)) return false;
    http::ServerOptions opts;
    opts.host = config_.get("server.host", "0.0.0.0");
    opts.port = static_cast<int>(config_.get_int("server.port", 8080));
    opts.workers = static_cast<size_t>(config_.get_int("server.workers", 16));
    opts.limits.max_body_bytes = static_cast<size_t>(config_.get_int("server.max_body_bytes", 10 * 1024 * 1024));
    opts.limits.max_header_bytes = static_cast<size_t>(config_.get_int("server.max_header_bytes", 64 * 1024));
    opts.max_requests_per_connection = static_cast<size_t>(config_.get_int("server.keepalive_requests", 100));
    server_ = std::make_unique<http::Server>(opts, [this](http::Request& req) { return handle(req); });
    return server_->start(error);
}

int Application::port() const {
    return server_ ? server_->port() : 0;
}

void Application::run_maintenance() {
    registry_->snapshot_if_needed(static_cast<uint64_t>(config_.get_int("data.snapshot_every", 5000)));
    const int64_t now = timeutil::now_ms();
    for (const auto& org : registry_->all()) records_->purge_expired(*org, now);
    public_limiter_->prune(now);
}

void Application::maintenance_loop() {
    std::unique_lock<std::mutex> lock(maintenance_mu_);
    while (!stopping_) {
        maintenance_cv_.wait_for(lock, std::chrono::seconds(60), [this] { return stopping_; });
        if (stopping_) break;
        lock.unlock();
        try {
            run_maintenance();
        } catch (const std::exception& e) {
            log::error("app", "maintenance failed: ", e.what());
        }
        lock.lock();
    }
}

void Application::shutdown() {
    {
        std::lock_guard<std::mutex> lock(maintenance_mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    maintenance_cv_.notify_all();
    if (maintenance_.joinable()) maintenance_.join();
    if (server_) server_->stop();
    if (bulk_) bulk_->stop();
    if (webhooks_) webhooks_->stop();
    if (registry_ && initialized_) {
        for (const auto& org : registry_->all()) {
            std::unique_lock<std::shared_mutex> lock(org->mutex());
            std::string err;
            if (org->db.persistent() && !org->db.snapshot(&err)) log::error("app", "final snapshot failed: ", err);
        }
    }
}

}  // namespace tcrm
