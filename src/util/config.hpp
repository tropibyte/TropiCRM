// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace tcrm {

/// Settings from an INI-style file ("[section]" headers, "key = value"
/// lines, '#' or ';' comments), addressed as "section.key". Environment
/// variables such as TROPICRM_SERVER_PORT override "server.port".
class Config {
public:
    Config() = default;

    /// Loads a file. Returns false and fills `error` if it can't be read or
    /// has a malformed line.
    bool load_file(const std::string& path, std::string* error);

    /// Parses INI text.
    bool load_string(std::string_view text, std::string* error);

    /// Applies overrides from the environment for every key that already has
    /// a value (so call it after loading defaults and the file).
    void apply_environment(std::string_view prefix = "TROPICRM_");

    void set(const std::string& key, std::string value) { values_[key] = std::move(value); }
    bool has(const std::string& key) const { return values_.count(key) != 0; }

    std::string get(const std::string& key, const std::string& fallback = {}) const;
    int64_t get_int(const std::string& key, int64_t fallback) const;
    bool get_bool(const std::string& key, bool fallback) const;

    const std::map<std::string, std::string>& values() const { return values_; }

    /// Defaults for every setting the server reads.
    static Config defaults();

private:
    std::map<std::string, std::string> values_;
};

}  // namespace tcrm
