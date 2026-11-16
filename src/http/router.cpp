// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "http/router.hpp"

#include <algorithm>

#include "util/url.hpp"

namespace tcrm::http {

std::vector<std::string> Router::split_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        if (path[start] == '/') {
            ++start;
            continue;
        }
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        parts.push_back(path.substr(start, end - start));
        start = end;
    }
    return parts;
}

void Router::add(Method method, const std::string& pattern, Handler handler) {
    Route r;
    r.method = method;
    r.pattern = pattern;
    for (const std::string& part : split_path(pattern)) {
        Segment s;
        if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
            s.capture = true;
            s.text = part.substr(1, part.size() - 2);
            if (!s.text.empty() && s.text[0] == '*') {
                s.rest = true;
                s.text.erase(0, 1);
            }
        } else {
            s.text = part;
        }
        r.segments.push_back(std::move(s));
    }
    r.handler = std::move(handler);
    routes_.push_back(std::move(r));
}

bool Router::match_route(const Route& route, const std::vector<std::string>& parts,
                         std::map<std::string, std::string>* params) {
    std::map<std::string, std::string> captured;
    size_t i = 0;
    for (size_t s = 0; s < route.segments.size(); ++s) {
        const Segment& seg = route.segments[s];
        if (seg.rest) {
            std::string rest;
            for (size_t j = i; j < parts.size(); ++j) {
                if (!rest.empty()) rest += '/';
                rest += url::percent_decode(parts[j]);
            }
            captured[seg.text] = rest;
            i = parts.size();
            break;
        }
        if (i >= parts.size()) return false;
        if (seg.capture) {
            captured[seg.text] = url::percent_decode(parts[i]);
        } else if (seg.text != parts[i]) {
            return false;
        }
        ++i;
    }
    if (i != parts.size()) return false;
    *params = std::move(captured);
    return true;
}

Router::Match Router::match(Request& req) const {
    Match m;
    const std::vector<std::string> parts = split_path(req.path);
    const Method want = req.method == Method::Head ? Method::Get : req.method;
    for (const Route& r : routes_) {
        std::map<std::string, std::string> params;
        if (!match_route(r, parts, &params)) continue;
        if (std::find(m.allowed.begin(), m.allowed.end(), r.method) == m.allowed.end()) {
            m.allowed.push_back(r.method);
        }
        if (r.method == want && m.handler == nullptr) {
            m.handler = &r.handler;
            m.pattern = r.pattern;
            req.path_params = std::move(params);
        }
    }
    return m;
}

std::vector<Router::RouteInfo> Router::routes() const {
    std::vector<RouteInfo> out;
    out.reserve(routes_.size());
    for (const Route& r : routes_) out.push_back({r.method, r.pattern});
    return out;
}

}  // namespace tcrm::http
