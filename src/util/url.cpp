// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/url.hpp"

#include "util/strutil.hpp"

namespace tcrm::url {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_unreserved(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

}  // namespace

std::string percent_decode(std::string_view s, bool plus_as_space) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '%' && i + 2 <= s.size()) {
            const int hi = hex_value(s[i + 1]);
            const int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (c == '+' && plus_as_space) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string percent_encode(std::string_view s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (is_unreserved(c)) {
            out += c;
        } else {
            const auto u = static_cast<unsigned char>(c);
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 15];
        }
    }
    return out;
}

Params parse_query(std::string_view qs) {
    Params out;
    if (!qs.empty() && qs[0] == '?') qs.remove_prefix(1);
    size_t start = 0;
    while (start <= qs.size()) {
        size_t amp = qs.find('&', start);
        if (amp == std::string_view::npos) amp = qs.size();
        const std::string_view pair = qs.substr(start, amp - start);
        if (!pair.empty()) {
            const size_t eq = pair.find('=');
            if (eq == std::string_view::npos) {
                out.emplace_back(percent_decode(pair, true), std::string());
            } else {
                out.emplace_back(percent_decode(pair.substr(0, eq), true), percent_decode(pair.substr(eq + 1), true));
            }
        }
        start = amp + 1;
    }
    return out;
}

std::string param(const Params& params, std::string_view key, std::string fallback) {
    for (const auto& [k, v] : params) {
        if (k == key) return v;
    }
    return fallback;
}

bool has_param(const Params& params, std::string_view key) {
    for (const auto& kv : params) {
        if (kv.first == key) return true;
    }
    return false;
}

bool parse_url(std::string_view text, Url* out) {
    text = str::trim(text);
    Url u;
    const size_t colon = text.find("://");
    if (colon == std::string_view::npos) return false;
    u.scheme = str::to_lower(text.substr(0, colon));
    if (u.scheme != "http" && u.scheme != "https") return false;
    std::string_view rest = text.substr(colon + 3);
    const size_t slash = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, slash);
    std::string_view tail = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
    if (authority.empty() || authority.find('@') != std::string_view::npos) return false;
    u.port = u.scheme == "https" ? 443 : 80;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        u.host = std::string(authority.substr(1, close - 1));
        authority.remove_prefix(close + 1);
        if (!authority.empty()) {
            if (authority.front() != ':') return false;
            int64_t port = 0;
            if (!str::parse_int64(authority.substr(1), &port) || port <= 0 || port > 65535) return false;
            u.port = static_cast<int>(port);
        }
    } else {
        const size_t pc = authority.rfind(':');
        if (pc != std::string_view::npos) {
            int64_t port = 0;
            if (!str::parse_int64(authority.substr(pc + 1), &port) || port <= 0 || port > 65535) return false;
            u.port = static_cast<int>(port);
            authority = authority.substr(0, pc);
        }
        u.host = str::to_lower(authority);
    }
    if (u.host.empty()) return false;
    const size_t hash = tail.find('#');
    if (hash != std::string_view::npos) tail = tail.substr(0, hash);
    u.path_and_query = tail.empty() ? "/" : std::string(tail);
    if (u.path_and_query[0] == '?') u.path_and_query.insert(0, "/");
    *out = std::move(u);
    return true;
}

}  // namespace tcrm::url
