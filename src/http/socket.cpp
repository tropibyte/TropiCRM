// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "http/socket.hpp"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace tcrm::net {
namespace {

#ifdef _WIN32
int close_socket(socket_t s) {
    return ::closesocket(static_cast<SOCKET>(s));
}
#else
int close_socket(socket_t s) {
    return ::close(s);
}
#endif

std::string addr_to_string(const sockaddr* sa) {
    char host[INET6_ADDRSTRLEN] = {};
    int port = 0;
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
        port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof host);
        port = ntohs(in6->sin6_port);
    } else {
        return "unknown";
    }
    return std::string(host) + ":" + std::to_string(port);
}

bool set_blocking(socket_t s, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
#endif
}

}  // namespace

bool initialize() {
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
#else
    return true;
#endif
}

std::string last_error_text() {
#ifdef _WIN32
    return "winsock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = kInvalidSocket;
    }
    return *this;
}

void Socket::close() {
    if (fd_ != kInvalidSocket) {
        close_socket(fd_);
        fd_ = kInvalidSocket;
    }
}

long Socket::recv(char* buf, size_t len) {
#ifdef _WIN32
    const int n = ::recv(static_cast<SOCKET>(fd_), buf, static_cast<int>(len), 0);
    return n == SOCKET_ERROR ? -1 : n;
#else
    while (true) {
        const ssize_t n = ::recv(fd_, buf, len, 0);
        if (n < 0 && errno == EINTR) continue;
        return n < 0 ? -1 : static_cast<long>(n);
    }
#endif
}

bool Socket::send_all(std::string_view data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const int n = ::send(static_cast<SOCKET>(fd_), data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n == SOCKET_ERROR) return false;
#else
        const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
#endif
        sent += static_cast<size_t>(n);
    }
    return true;
}

void Socket::shutdown_both() {
    if (!valid()) return;
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(fd_), SD_BOTH);
#else
    ::shutdown(fd_, SHUT_RDWR);
#endif
}

bool Socket::set_recv_timeout(int ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
    return setsockopt(static_cast<SOCKET>(fd_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
                      sizeof tv) == 0;
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0;
#endif
}

bool Socket::set_send_timeout(int ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
    return setsockopt(static_cast<SOCKET>(fd_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
                      sizeof tv) == 0;
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
#endif
}

bool Socket::set_nodelay(bool on) {
    int v = on ? 1 : 0;
    return setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v), sizeof v) == 0;
}

Socket listen_tcp(const std::string& host, int port, int backlog, std::string* error, int* bound_port) {
    initialize();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(), port_text.c_str(), &hints, &res);
    if (rc != 0) {
        if (error) *error = std::string("getaddrinfo: ") + gai_strerror(rc);
        return Socket();
    }
    Socket result;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        Socket s(static_cast<socket_t>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)));
        if (!s.valid()) continue;
        int one = 1;
        setsockopt(s.fd(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        if (::bind(s.fd(), ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0) continue;
        if (::listen(s.fd(), backlog) != 0) continue;
        result = std::move(s);
        break;
    }
    freeaddrinfo(res);
    if (!result.valid()) {
        if (error) *error = "cannot listen on " + host + ":" + port_text + ": " + last_error_text();
        return result;
    }
    if (bound_port) {
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
        if (getsockname(result.fd(), reinterpret_cast<sockaddr*>(&ss), &len) == 0) {
            if (ss.ss_family == AF_INET) {
                *bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                *bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
            }
        }
    }
    return result;
}

Socket accept_connection(const Socket& listener, std::string* remote_addr) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    while (true) {
        const auto fd = ::accept(listener.fd(), reinterpret_cast<sockaddr*>(&ss), &len);
#ifdef _WIN32
        if (fd == INVALID_SOCKET) return Socket();
#else
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return Socket();
        }
#endif
        if (remote_addr) *remote_addr = addr_to_string(reinterpret_cast<sockaddr*>(&ss));
        return Socket(static_cast<socket_t>(fd));
    }
}

Socket connect_tcp(const std::string& host, int port, int timeout_ms, std::string* error) {
    initialize();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &res);
    if (rc != 0) {
        if (error) *error = std::string("cannot resolve ") + host + ": " + gai_strerror(rc);
        return Socket();
    }
    Socket result;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        Socket s(static_cast<socket_t>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)));
        if (!s.valid()) continue;
        set_blocking(s.fd(), false);
        int c = ::connect(s.fd(), ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool connected = c == 0;
        if (!connected) {
#ifdef _WIN32
            const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool pending = errno == EINPROGRESS;
#endif
            if (pending) {
#ifdef _WIN32
                WSAPOLLFD pfd{};
                pfd.fd = static_cast<SOCKET>(s.fd());
                pfd.events = POLLOUT;
                const int pr = WSAPoll(&pfd, 1, timeout_ms);
#else
                pollfd pfd{};
                pfd.fd = s.fd();
                pfd.events = POLLOUT;
                const int pr = ::poll(&pfd, 1, timeout_ms);
#endif
                if (pr > 0) {
                    int err = 0;
                    socklen_t len = sizeof err;
                    getsockopt(s.fd(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                    connected = err == 0;
                }
            }
        }
        if (!connected) continue;
        set_blocking(s.fd(), true);
        s.set_recv_timeout(timeout_ms);
        s.set_send_timeout(timeout_ms);
        result = std::move(s);
        break;
    }
    freeaddrinfo(res);
    if (!result.valid() && error) *error = "cannot connect to " + host + ":" + port_text;
    return result;
}

}  // namespace tcrm::net
