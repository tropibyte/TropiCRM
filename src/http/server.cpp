// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "http/server.hpp"

#include <exception>

#include "util/log.hpp"
#include "util/strutil.hpp"

namespace tcrm::http {
namespace {

Response error_response(int status, const std::string& message) {
    json::Value body = json::Value::array();
    json::Value err = json::Value::object();
    err.set("errorCode", status == 413 ? "REQUEST_TOO_LARGE" : "BAD_REQUEST");
    err.set("message", message);
    body.push_back(std::move(err));
    Response r = Response::json(status, body);
    r.close_connection = true;
    return r;
}

}  // namespace

Server::Server(ServerOptions options, Handler handler) : options_(std::move(options)), handler_(std::move(handler)) {}

Server::~Server() {
    stop();
}

bool Server::start(std::string* error) {
    if (running_) return true;
    if (!net::initialize()) {
        if (error) *error = "socket library failed to initialize";
        return false;
    }
    listener_ = net::listen_tcp(options_.host, options_.port, 128, error, &bound_port_);
    if (!listener_.valid()) return false;
    pool_ = std::make_unique<ThreadPool>(options_.workers, "http");
    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
    log::info("http", "listening on ", options_.host, ":", bound_port_, " with ", options_.workers, " workers");
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    listener_.shutdown_both();
    listener_.close();
    if (accept_thread_.joinable()) accept_thread_.join();
    if (pool_) pool_->shutdown();
    log::info("http", "stopped after ", requests_.load(), " requests");
}

void Server::accept_loop() {
    while (running_) {
        std::string remote;
        net::Socket conn = net::accept_connection(listener_, &remote);
        if (!conn.valid()) {
            if (!running_) break;
            continue;
        }
        ++connections_;
        conn.set_nodelay(true);
        auto shared = std::make_shared<net::Socket>(std::move(conn));
        const bool queued = pool_->submit([this, shared, remote] { serve(std::move(*shared), remote); });
        if (!queued) {
            shared->send_all(error_response(503, "server is shutting down").serialize(false));
        }
    }
}

void Server::serve(net::Socket sock, std::string remote) {
    RequestParser parser(options_.limits);
    std::string buffer;
    char chunk[16 * 1024];
    size_t served = 0;
    while (running_) {
        RequestParser::Result r = parser.parse(buffer);
        while (r == RequestParser::Result::NeedMore) {
            const long n = sock.recv(chunk, sizeof chunk);
            if (n <= 0) return;  // closed, reset or timed out
            buffer.append(chunk, static_cast<size_t>(n));
            r = parser.parse(buffer);
        }
        if (r == RequestParser::Result::Error) {
            sock.send_all(error_response(parser.error_status(), parser.error_message()).serialize(false));
            return;
        }

        Request req = parser.take();
        req.remote_addr = remote;
        req.request_id = "req-" + std::to_string(next_request_id_.fetch_add(1));
        const bool head = req.method == Method::Head;
        Response resp;
        try {
            resp = handler_(req);
        } catch (const std::exception& e) {
            log::error("http", req.request_id, " unhandled exception: ", e.what());
            resp = error_response(500, "internal server error");
        }
        ++served;
        ++requests_;
        if (!req.keep_alive() || served >= options_.max_requests_per_connection) resp.close_connection = true;
        if (!resp.headers.has("X-Request-Id")) resp.headers.set("X-Request-Id", req.request_id);
        if (!sock.send_all(resp.serialize(head))) return;
        if (resp.close_connection) {
            sock.shutdown_both();
            return;
        }
    }
}

}  // namespace tcrm::http
