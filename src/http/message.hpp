// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <any>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json/json.hpp"
#include "util/url.hpp"

namespace tcrm::http {

/// Header fields with case-insensitive names, kept in arrival order.
class Headers {
public:
    using Field = std::pair<std::string, std::string>;

    void add(std::string name, std::string value);
    /// Replaces every field with this name.
    void set(std::string name, std::string value);
    void remove(std::string_view name);

    /// First value for `name`, or `fallback`.
    std::string get(std::string_view name, const std::string& fallback = {}) const;
    bool has(std::string_view name) const;
    size_t count(std::string_view name) const;

    const std::vector<Field>& fields() const { return fields_; }
    size_t size() const { return fields_.size(); }

private:
    std::vector<Field> fields_;
};

enum class Method { Get, Head, Post, Put, Patch, Delete, Options, Unknown };

Method parse_method(std::string_view text);
const char* method_name(Method m);

struct Request {
    Method method = Method::Unknown;
    std::string method_text;
    std::string target;        // as received: path plus query
    std::string path;          // percent-encoded path, no query
    std::string query_string;  // without the '?'
    url::Params query;
    std::string version = "HTTP/1.1";
    Headers headers;
    std::string body;

    // Filled in by the router and middleware.
    std::map<std::string, std::string> path_params;
    std::string remote_addr;
    std::string request_id;
    std::any auth;  // the authenticated principal, if any

    const std::string& path_param(const std::string& name) const;
    std::string query_param(std::string_view name, const std::string& fallback = {}) const;
    bool has_query_param(std::string_view name) const;

    /// True when the connection should stay open after this request.
    bool keep_alive() const;

    /// Parses the body as JSON; throws json::ParseError.
    json::Value json_body() const;
};

struct Response {
    int status = 200;
    Headers headers;
    std::string body;
    bool close_connection = false;

    static Response json(int status, const json::Value& value);
    static Response text(int status, std::string body, std::string content_type = "text/plain; charset=utf-8");
    static Response no_content();

    /// Serializes status line, headers and body. Adds Content-Length,
    /// Date and Connection.
    std::string serialize(bool omit_body) const;
};

const char* reason_phrase(int status);

}  // namespace tcrm::http
