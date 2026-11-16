// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "http/message.hpp"

namespace tcrm::api {

/// Token-bucket rate limiting keyed by an arbitrary string (client address,
/// user id...). Thread-safe.
class RateLimiter {
public:
    /// `capacity` requests may burst; the bucket refills at
    /// `refill_per_minute` tokens per minute.
    RateLimiter(double capacity, double refill_per_minute) : capacity_(capacity), refill_per_ms_(refill_per_minute / 60000.0) {}

    /// Takes one token for `key`. Returns false when the bucket is empty;
    /// `retry_after_ms` then says when the next token arrives.
    bool allow(const std::string& key, int64_t now_ms, int64_t* retry_after_ms = nullptr);

    size_t tracked_keys() const;

    /// Drops buckets that have been full for a while.
    void prune(int64_t now_ms);

private:
    struct Bucket {
        double tokens = 0;
        int64_t updated_ms = 0;
    };

    double capacity_;
    double refill_per_ms_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Bucket> buckets_;
};

/// The client's address for logging and rate limiting. Honors
/// X-Forwarded-For so deployments behind a load balancer see real clients.
std::string client_ip(const http::Request& req);

}  // namespace tcrm::api
