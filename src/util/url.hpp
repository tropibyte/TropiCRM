// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tcrm::url {

using Params = std::vector<std::pair<std::string, std::string>>;

/// Decodes %XX escapes. Malformed escapes are kept literally. With
/// `plus_as_space`, '+' decodes to a space (form encoding).
std::string percent_decode(std::string_view s, bool plus_as_space = false);

/// Encodes everything except RFC 3986 unreserved characters.
std::string percent_encode(std::string_view s);

/// Parses "a=1&b=two&flag" into ordered pairs, decoding both sides.
Params parse_query(std::string_view qs);

/// First value for `key`, or `fallback`.
std::string param(const Params& params, std::string_view key, std::string fallback = {});
bool has_param(const Params& params, std::string_view key);

struct Url {
    std::string scheme;  // "http" or "https"
    std::string host;
    int port = 0;
    std::string path_and_query;  // starts with '/'
};

/// Parses absolute http(s) URLs. Userinfo is not supported.
bool parse_url(std::string_view text, Url* out);

}  // namespace tcrm::url
