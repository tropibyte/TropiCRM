// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>

#include "http/message.hpp"
#include "util/url.hpp"

namespace tcrm::http {

struct ClientResponse {
    int status = 0;
    Headers headers;
    std::string body;
};

struct ClientOptions {
    int timeout_ms = 5000;
};

/// Sends one request over a fresh connection and reads the whole response.
/// Only plain http:// is supported; deployments that need TLS put an
/// egress relay in front.
bool send_request(Method method, const url::Url& target, const Headers& headers, const std::string& body,
                  const ClientOptions& options, ClientResponse* out, std::string* error);

}  // namespace tcrm::http
