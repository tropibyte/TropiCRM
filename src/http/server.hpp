// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "http/message.hpp"
#include "http/parser.hpp"
#include "http/socket.hpp"
#include "util/thread_pool.hpp"

namespace tcrm::http {

struct ServerOptions {
    std::string host = "0.0.0.0";
    int port = 8080;
    size_t workers = 16;
    ParserLimits limits;
    size_t max_requests_per_connection = 100;
};

/// A blocking HTTP/1.1 server: one accept thread hands each connection to
/// a worker pool, and a worker serves that connection's keep-alive
/// requests until the client closes it.
class Server {
public:
    using Handler = std::function<Response(Request&)>;

    Server(ServerOptions options, Handler handler);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(std::string* error);
    void stop();

    /// The bound port (useful when started with port 0).
    int port() const { return bound_port_; }

    uint64_t requests_served() const { return requests_.load(); }
    uint64_t connections_accepted() const { return connections_.load(); }

private:
    void accept_loop();
    void serve(net::Socket sock, std::string remote);

    ServerOptions options_;
    Handler handler_;
    net::Socket listener_;
    std::unique_ptr<ThreadPool> pool_;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
    int bound_port_ = 0;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> connections_{0};
    std::atomic<uint64_t> next_request_id_{1};
};

}  // namespace tcrm::http
