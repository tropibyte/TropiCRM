// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "http/message.hpp"

#include <algorithm>

#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::http {

void Headers::add(std::string name, std::string value) {
    fields_.emplace_back(std::move(name), std::move(value));
}

void Headers::set(std::string name, std::string value) {
    remove(name);
    fields_.emplace_back(std::move(name), std::move(value));
}

void Headers::remove(std::string_view name) {
    fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                 [&](const Field& f) { return str::iequals(f.first, name); }),
                  fields_.end());
}

std::string Headers::get(std::string_view name, const std::string& fallback) const {
    for (const Field& f : fields_) {
        if (str::iequals(f.first, name)) return f.second;
    }
    return fallback;
}

bool Headers::has(std::string_view name) const {
    return count(name) > 0;
}

size_t Headers::count(std::string_view name) const {
    size_t n = 0;
    for (const Field& f : fields_) {
        if (str::iequals(f.first, name)) ++n;
    }
    return n;
}

Method parse_method(std::string_view text) {
    if (text == "GET") return Method::Get;
    if (text == "HEAD") return Method::Head;
    if (text == "POST") return Method::Post;
    if (text == "PUT") return Method::Put;
    if (text == "PATCH") return Method::Patch;
    if (text == "DELETE") return Method::Delete;
    if (text == "OPTIONS") return Method::Options;
    return Method::Unknown;
}

const char* method_name(Method m) {
    switch (m) {
        case Method::Get: return "GET";
        case Method::Head: return "HEAD";
        case Method::Post: return "POST";
        case Method::Put: return "PUT";
        case Method::Patch: return "PATCH";
        case Method::Delete: return "DELETE";
        case Method::Options: return "OPTIONS";
        case Method::Unknown: break;
    }
    return "UNKNOWN";
}

const std::string& Request::path_param(const std::string& name) const {
    static const std::string kEmpty;
    auto it = path_params.find(name);
    return it == path_params.end() ? kEmpty : it->second;
}

std::string Request::query_param(std::string_view name, const std::string& fallback) const {
    return url::param(query, name, fallback);
}

bool Request::has_query_param(std::string_view name) const {
    return url::has_param(query, name);
}

bool Request::keep_alive() const {
    const std::string conn = str::to_lower(headers.get("Connection"));
    if (version == "HTTP/1.0") return conn.find("keep-alive") != std::string::npos;
    return conn.find("close") == std::string::npos;
}

json::Value Request::json_body() const {
    if (str::trim(body).empty()) return json::Value::object();
    return json::parse(body);
}

Response Response::json(int status, const json::Value& value) {
    Response r;
    r.status = status;
    r.headers.set("Content-Type", "application/json; charset=utf-8");
    r.body = value.dump();
    return r;
}

Response Response::text(int status, std::string body, std::string content_type) {
    Response r;
    r.status = status;
    r.headers.set("Content-Type", std::move(content_type));
    r.body = std::move(body);
    return r;
}

Response Response::no_content() {
    Response r;
    r.status = 204;
    return r;
}

std::string Response::serialize(bool omit_body) const {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) + "\r\n";
    for (const auto& [name, value] : headers.fields()) {
        if (str::iequals(name, "Content-Length") || str::iequals(name, "Connection")) continue;
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    if (!headers.has("Date")) {
        out += "Date: " + timeutil::format_datetime(timeutil::now_ms()) + "\r\n";
    }
    if (status != 204 && status != 304) {
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    out += close_connection ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
    out += "\r\n";
    if (!omit_body && status != 204 && status != 304) out += body;
    return out;
}

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

}  // namespace tcrm::http
