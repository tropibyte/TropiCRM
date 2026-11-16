// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/id.hpp"

namespace tcrm::ids {
namespace {

constexpr char kBase62[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr char kSuffixChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";

bool is_base62(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int base62_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    return -1;
}

}  // namespace

std::string suffix_for(std::string_view id15) {
    std::string out;
    for (size_t chunk = 0; chunk < 3; ++chunk) {
        int flags = 0;
        for (size_t i = 0; i < 5; ++i) {
            const char c = id15[chunk * 5 + i];
            if (c >= 'A' && c <= 'Z') flags |= 1 << i;
        }
        out += kSuffixChars[flags];
    }
    return out;
}

bool normalize(std::string_view id, std::string* out18) {
    if (id.size() != kShortLength && id.size() != kLongLength) return false;
    for (char c : id) {
        if (!is_base62(c)) return false;
    }
    if (id.size() == kShortLength) {
        *out18 = std::string(id) + suffix_for(id);
        return true;
    }
    // 18-character ids may arrive with their case mangled (spreadsheets
    // love to do that); the suffix tells us the original case.
    std::string id15(id.substr(0, kShortLength));
    for (size_t chunk = 0; chunk < 3; ++chunk) {
        const char s = id[kShortLength + chunk];
        int flags = -1;
        for (int i = 0; i < 32; ++i) {
            if (kSuffixChars[i] == (s >= 'a' && s <= 'z' ? static_cast<char>(s - 'a' + 'A') : s)) {
                flags = i;
                break;
            }
        }
        if (flags < 0) return false;
        for (size_t i = 0; i < 5; ++i) {
            char& c = id15[chunk * 5 + i];
            const bool upper = (flags >> i) & 1;
            if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            if (!upper && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
    }
    *out18 = id15 + suffix_for(id15);
    return true;
}

std::string_view key_prefix(std::string_view id) {
    if (id.size() != kShortLength && id.size() != kLongLength) return {};
    return id.substr(0, 3);
}

bool looks_like_id(std::string_view s) {
    std::string tmp;
    return normalize(s, &tmp);
}

std::string encode_base62(uint64_t v, size_t width) {
    std::string out(width, '0');
    for (size_t i = width; i > 0 && v > 0; --i) {
        out[i - 1] = kBase62[v % 62];
        v /= 62;
    }
    return out;
}

bool decode_base62(std::string_view s, uint64_t* out) {
    uint64_t v = 0;
    for (char c : s) {
        const int d = base62_value(c);
        if (d < 0) return false;
        v = v * 62 + static_cast<uint64_t>(d);
    }
    *out = v;
    return true;
}

std::string Generator::next(std::string_view key_prefix) {
    const uint64_t n = next_.fetch_add(1);
    std::string id15;
    id15.reserve(kLongLength);
    id15.append(key_prefix.substr(0, 3));
    id15.append(pod_.substr(0, 2));
    id15.append(encode_base62(n, 10));
    return id15 + suffix_for(id15);
}

void Generator::observe(std::string_view id) {
    if (id.size() < kShortLength) return;
    uint64_t n = 0;
    if (!decode_base62(id.substr(5, 10), &n)) return;
    uint64_t cur = next_.load();
    while (cur <= n && !next_.compare_exchange_weak(cur, n + 1)) {
    }
}

}  // namespace tcrm::ids
