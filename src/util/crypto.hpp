// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace tcrm::crypto {

using Digest = std::array<uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    Digest finish();

private:
    void compress(const uint8_t* block);

    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_len_ = 0;
};

Digest sha256(std::string_view data);
Digest hmac_sha256(std::string_view key, std::string_view message);

/// PBKDF2-HMAC-SHA256 producing `out_len` bytes.
std::string pbkdf2_sha256(std::string_view password, std::string_view salt, uint32_t iterations, size_t out_len);

/// Compares two strings in time that depends only on their lengths.
bool constant_time_equals(std::string_view a, std::string_view b);

std::string to_hex(std::string_view bytes);
std::string to_hex(const Digest& d);
bool from_hex(std::string_view hex, std::string* out);

std::string base64_encode(std::string_view bytes);
bool base64_decode(std::string_view text, std::string* out);

/// URL-safe alphabet, no padding.
std::string base64url_encode(std::string_view bytes);
bool base64url_decode(std::string_view text, std::string* out);

inline std::string digest_bytes(const Digest& d) {
    return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

/// Bytes from the operating system's random source.
std::string random_bytes(size_t n);

/// A random token of `n_bytes` bytes, base64url-encoded.
std::string random_token(size_t n_bytes = 32);

}  // namespace tcrm::crypto
