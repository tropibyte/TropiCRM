// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "util/strutil.hpp"

namespace tcrm {

bool Config::load_file(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return load_string(ss.str(), error);
}

bool Config::load_string(std::string_view text, std::string* error) {
    std::string section;
    size_t line_no = 0;
    for (const std::string& raw : str::split(text, '\n')) {
        ++line_no;
        std::string_view line = str::trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            if (line.back() != ']') {
                if (error) *error = "line " + std::to_string(line_no) + ": bad section header";
                return false;
            }
            section = str::to_lower(str::trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (error) *error = "line " + std::to_string(line_no) + ": expected key = value";
            return false;
        }
        std::string key = str::to_lower(str::trim(line.substr(0, eq)));
        std::string_view value = str::trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (key.empty()) {
            if (error) *error = "line " + std::to_string(line_no) + ": empty key";
            return false;
        }
        values_[section.empty() ? key : section + "." + key] = std::string(value);
    }
    return true;
}

void Config::apply_environment(std::string_view prefix) {
    for (auto& [key, value] : values_) {
        std::string env_name(prefix);
        for (char c : key) env_name += (c == '.') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (const char* v = std::getenv(env_name.c_str())) value = v;
    }
}

std::string Config::get(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

int64_t Config::get_int(const std::string& key, int64_t fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    int64_t v = 0;
    return str::parse_int64(str::trim(it->second), &v) ? v : fallback;
}

bool Config::get_bool(const std::string& key, bool fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    const std::string v = str::to_lower(str::trim(it->second));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

Config Config::defaults() {
    Config c;
    c.set("server.host", "0.0.0.0");
    c.set("server.port", "8080");
    c.set("server.workers", "16");
    c.set("server.max_body_bytes", "10485760");
    c.set("server.max_header_bytes", "65536");
    c.set("server.keepalive_requests", "100");
    c.set("data.dir", "./data");
    c.set("data.snapshot_every", "5000");
    c.set("auth.token_ttl_seconds", "7200");
    c.set("auth.refresh_ttl_days", "30");
    c.set("auth.pbkdf2_iterations", "100000");
    c.set("auth.max_failed_logins", "5");
    c.set("auth.lockout_minutes", "15");
    c.set("auth.password_reset_ttl_minutes", "60");
    c.set("limits.api_calls_per_day", "15000");
    c.set("limits.query_max_rows", "2000");
    c.set("limits.storage_mb", "1024");
    c.set("limits.public_requests_per_minute", "30");
    c.set("webhooks.workers", "2");
    c.set("webhooks.timeout_ms", "5000");
    c.set("webhooks.max_attempts", "10");
    c.set("cors.allowed_origins", "");
    c.set("log.level", "info");
    c.set("log.file", "");
    c.set("log.requests", "true");
    c.set("platform.admin_key_sha256", "");
    c.set("platform.dev_mode", "false");
    c.set("platform.signup_enabled", "true");
    return c;
}

}  // namespace tcrm
