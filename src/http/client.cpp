// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "http/client.hpp"

#include "http/socket.hpp"
#include "util/strutil.hpp"

namespace tcrm::http {
namespace {

bool parse_status_line(std::string_view line, int* status) {
    // HTTP/1.1 200 OK
    if (!str::istarts_with(line, "HTTP/")) return false;
    const size_t sp = line.find(' ');
    if (sp == std::string_view::npos || sp + 4 > line.size()) return false;
    int64_t v = 0;
    if (!str::parse_int64(line.substr(sp + 1, 3), &v)) return false;
    *status = static_cast<int>(v);
    return true;
}

bool decode_chunked(std::string_view in, std::string* out) {
    size_t pos = 0;
    out->clear();
    while (pos < in.size()) {
        const size_t eol = in.find("\r\n", pos);
        if (eol == std::string_view::npos) return false;
        std::string_view size_line = in.substr(pos, eol - pos);
        const size_t semi = size_line.find(';');
        if (semi != std::string_view::npos) size_line = size_line.substr(0, semi);
        size_t size = 0;
        for (char c : str::trim(size_line)) {
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) return false;
            size = size * 16 + static_cast<size_t>(d);
        }
        pos = eol + 2;
        if (size == 0) return true;
        if (pos + size > in.size()) return false;
        out->append(in.substr(pos, size));
        pos += size + 2;
    }
    return false;
}

}  // namespace

bool send_request(Method method, const url::Url& target, const Headers& headers, const std::string& body,
                  const ClientOptions& options, ClientResponse* out, std::string* error) {
    if (target.scheme != "http") {
        if (error) *error = "only http:// targets are supported";
        return false;
    }
    net::Socket sock = net::connect_tcp(target.host, target.port, options.timeout_ms, error);
    if (!sock.valid()) return false;

    std::string req = std::string(method_name(method)) + " " + target.path_and_query + " HTTP/1.1\r\n";
    req += "Host: " + target.host;
    if (target.port != 80) req += ":" + std::to_string(target.port);
    req += "\r\n";
    for (const auto& [name, value] : headers.fields()) {
        req += name + ": " + value + "\r\n";
    }
    if (!headers.has("User-Agent")) req += "User-Agent: TropiCRM/1.0\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    if (!sock.send_all(req)) {
        if (error) *error = "send failed: " + net::last_error_text();
        return false;
    }

    std::string raw;
    char buf[8192];
    while (true) {
        const long n = sock.recv(buf, sizeof buf);
        if (n < 0) {
            if (raw.empty()) {
                if (error) *error = "no response (timeout or reset)";
                return false;
            }
            break;
        }
        if (n == 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }

    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        if (error) *error = "malformed response";
        return false;
    }
    const std::string_view head(raw.data(), head_end);
    const size_t first_eol = head.find("\r\n");
    ClientResponse resp;
    if (!parse_status_line(head.substr(0, first_eol), &resp.status)) {
        if (error) *error = "malformed status line";
        return false;
    }
    size_t pos = first_eol == std::string_view::npos ? head.size() : first_eol + 2;
    while (pos < head.size()) {
        size_t eol = head.find("\r\n", pos);
        if (eol == std::string_view::npos) eol = head.size();
        const std::string_view line = head.substr(pos, eol - pos);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            resp.headers.add(std::string(str::trim(line.substr(0, colon))), std::string(str::trim(line.substr(colon + 1))));
        }
        pos = eol + 2;
    }
    std::string_view payload = std::string_view(raw).substr(head_end + 4);
    if (str::iequals(resp.headers.get("Transfer-Encoding"), "chunked")) {
        if (!decode_chunked(payload, &resp.body)) {
            if (error) *error = "malformed chunked body";
            return false;
        }
    } else {
        resp.body = std::string(payload);
    }
    *out = std::move(resp);
    return true;
}

}  // namespace tcrm::http
