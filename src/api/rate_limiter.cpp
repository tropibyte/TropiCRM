// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "api/rate_limiter.hpp"

#include <algorithm>
#include <cmath>

#include "util/strutil.hpp"

namespace tcrm::api {

bool RateLimiter::allow(const std::string& key, int64_t now_ms, int64_t* retry_after_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, inserted] = buckets_.try_emplace(key);
    Bucket& b = it->second;
    if (inserted) {
        b.tokens = capacity_;
        b.updated_ms = now_ms;
    }
    const double elapsed = static_cast<double>(std::max<int64_t>(0, now_ms - b.updated_ms));
    b.tokens = std::min(capacity_, b.tokens + elapsed * refill_per_ms_);
    b.updated_ms = now_ms;
    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    if (retry_after_ms != nullptr) {
        *retry_after_ms = static_cast<int64_t>(std::ceil((1.0 - b.tokens) / refill_per_ms_));
    }
    return false;
}

size_t RateLimiter::tracked_keys() const {
    std::lock_guard<std::mutex> lock(mu_);
    return buckets_.size();
}

void RateLimiter::prune(int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        const double refilled = it->second.tokens + static_cast<double>(now_ms - it->second.updated_ms) * refill_per_ms_;
        if (refilled >= capacity_) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string client_ip(const http::Request& req) {
    const std::string forwarded = req.headers.get("X-Forwarded-For");
    if (!forwarded.empty()) {
        const std::string first = str::split(forwarded, ',')[0];
        return std::string(str::trim(first));
    }
    const size_t colon = req.remote_addr.rfind(':');
    return colon == std::string::npos ? req.remote_addr : req.remote_addr.substr(0, colon);
}

}  // namespace tcrm::api
