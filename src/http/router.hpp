// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "http/message.hpp"

namespace tcrm::http {

using Handler = std::function<Response(Request&)>;

/// Maps method + path pattern to handlers. Patterns are literal segments,
/// "{name}" captures one segment and a trailing "{*name}" captures the rest
/// of the path. Captured segments are percent-decoded.
class Router {
public:
    void add(Method method, const std::string& pattern, Handler handler);

    void get(const std::string& p, Handler h) { add(Method::Get, p, std::move(h)); }
    void post(const std::string& p, Handler h) { add(Method::Post, p, std::move(h)); }
    void put(const std::string& p, Handler h) { add(Method::Put, p, std::move(h)); }
    void patch(const std::string& p, Handler h) { add(Method::Patch, p, std::move(h)); }
    void del(const std::string& p, Handler h) { add(Method::Delete, p, std::move(h)); }

    struct Match {
        const Handler* handler = nullptr;
        std::vector<Method> allowed;  // methods registered for the path
        std::string pattern;
    };

    /// Finds the handler and fills req.path_params. HEAD falls back to GET.
    Match match(Request& req) const;

    size_t route_count() const { return routes_.size(); }

    struct RouteInfo {
        Method method;
        std::string pattern;
    };
    std::vector<RouteInfo> routes() const;

private:
    struct Segment {
        std::string text;
        bool capture = false;
        bool rest = false;
    };
    struct Route {
        Method method;
        std::string pattern;
        std::vector<Segment> segments;
        Handler handler;
    };

    static std::vector<std::string> split_path(const std::string& path);
    static bool match_route(const Route& route, const std::vector<std::string>& parts,
                            std::map<std::string, std::string>* params);

    std::vector<Route> routes_;
};

}  // namespace tcrm::http
