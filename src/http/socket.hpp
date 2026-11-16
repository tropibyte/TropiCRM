// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tcrm::net {

#ifdef _WIN32
using socket_t = uintptr_t;
constexpr socket_t kInvalidSocket = ~static_cast<socket_t>(0);
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

/// Initializes the socket library (WSAStartup on Windows). Safe to call
/// more than once.
bool initialize();

/// An owned TCP socket. Move-only.
class Socket {
public:
    Socket() = default;
    explicit Socket(socket_t fd) : fd_(fd) {}
    ~Socket() { close(); }

    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidSocket; }
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return fd_ != kInvalidSocket; }
    socket_t fd() const { return fd_; }
    void close();

    /// Bytes read (> 0), 0 when the peer closed, -1 on error or timeout.
    long recv(char* buf, size_t len);

    /// Sends everything or returns false.
    bool send_all(std::string_view data);

    void shutdown_both();
    bool set_recv_timeout(int ms);
    bool set_send_timeout(int ms);
    bool set_nodelay(bool on);

private:
    socket_t fd_ = kInvalidSocket;
};

/// Binds and listens. `port` 0 picks a free port, reported in `bound_port`.
Socket listen_tcp(const std::string& host, int port, int backlog, std::string* error, int* bound_port);

/// Blocks for the next connection. Returns an invalid socket when the
/// listener was closed.
Socket accept_connection(const Socket& listener, std::string* remote_addr);

/// Connects with a timeout.
Socket connect_tcp(const std::string& host, int port, int timeout_ms, std::string* error);

std::string last_error_text();

}  // namespace tcrm::net
