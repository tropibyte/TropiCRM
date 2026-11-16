// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace tcrm::ids {

// Record ids follow the familiar 15/18-character scheme:
//   chars 0-2   key prefix of the object ("001" for Account)
//   chars 3-4   instance ("pod") code of the org that created it
//   chars 5-14  base-62 sequence number
//   chars 15-17 case-safe checksum (18-character form only)
// The 18-character form is canonical everywhere in the API.

constexpr size_t kShortLength = 15;
constexpr size_t kLongLength = 18;

/// Computes the 3-character case-safety suffix for a 15-character id.
std::string suffix_for(std::string_view id15);

/// Accepts a 15- or 18-character id and returns its 18-character form.
/// Returns false for anything that is not a well-formed id.
bool normalize(std::string_view id, std::string* out18);

/// The 3-character key prefix, or "" for a malformed id.
std::string_view key_prefix(std::string_view id);

bool looks_like_id(std::string_view s);

std::string encode_base62(uint64_t v, size_t width);
bool decode_base62(std::string_view s, uint64_t* out);

/// Hands out ids for one org. Thread-safe.
class Generator {
public:
    explicit Generator(std::string pod = "0A", uint64_t next = 1) : pod_(std::move(pod)), next_(next) {}

    std::string next(std::string_view key_prefix);

    /// Makes sure future ids sort after `id`; used when replaying a log.
    void observe(std::string_view id);

    uint64_t peek() const { return next_.load(); }
    const std::string& pod() const { return pod_; }

private:
    std::string pod_;
    std::atomic<uint64_t> next_;
};

}  // namespace tcrm::ids
