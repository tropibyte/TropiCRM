// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/crypto.hpp"

#include <algorithm>
#include <cstring>
#include <random>

namespace tcrm::crypto {
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64_encode(std::string_view in, const char* alphabet, bool pad) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) |
                           static_cast<uint8_t>(in[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
        i += 3;
    }
    const size_t rest = in.size() - i;
    if (rest == 1) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        if (pad) out += "==";
    } else if (rest == 2) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        if (pad) out += '=';
    }
    return out;
}

int b64_value(char c, bool url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (!url && c == '+') return 62;
    if (!url && c == '/') return 63;
    if (url && c == '-') return 62;
    if (url && c == '_') return 63;
    return -1;
}

bool b64_decode(std::string_view in, bool url, std::string* out) {
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);
    if (in.size() % 4 == 1) return false;
    std::string result;
    result.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        const int v = b64_value(c, url);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    *out = std::move(result);
    return true;
}

}  // namespace

Sha256::Sha256() {
    h_[0] = 0x6a09e667;
    h_[1] = 0xbb67ae85;
    h_[2] = 0x3c6ef372;
    h_[3] = 0xa54ff53a;
    h_[4] = 0x510e527f;
    h_[5] = 0x9b05688c;
    h_[6] = 0x1f83d9ab;
    h_[7] = 0x5be0cd19;
}

void Sha256::compress(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_len_ += len;
    if (buf_len_ > 0) {
        const size_t take = std::min(len, sizeof buf_ - buf_len_);
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ == sizeof buf_) {
            compress(buf_);
            buf_len_ = 0;
        }
    }
    while (len >= 64) {
        compress(p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(buf_, p, len);
        buf_len_ = len;
    }
}

Digest Sha256::finish() {
    const uint64_t bit_len = total_len_ * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (buf_len_ != 56) update(&zero, 1);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) len_bytes[i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
    update(len_bytes, 8);
    Digest out;
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
    return out;
}

Digest sha256(std::string_view data) {
    Sha256 h;
    h.update(data);
    return h.finish();
}

Digest hmac_sha256(std::string_view key, std::string_view message) {
    uint8_t k[64] = {};
    if (key.size() > 64) {
        const Digest kd = sha256(key);
        std::memcpy(k, kd.data(), kd.size());
    } else {
        std::memcpy(k, key.data(), key.size());
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256 inner;
    inner.update(ipad, 64);
    inner.update(message);
    const Digest ih = inner.finish();
    Sha256 outer;
    outer.update(opad, 64);
    outer.update(ih.data(), ih.size());
    return outer.finish();
}

std::string pbkdf2_sha256(std::string_view password, std::string_view salt, uint32_t iterations, size_t out_len) {
    std::string out;
    uint32_t block = 1;
    while (out.size() < out_len) {
        std::string s(salt);
        s += static_cast<char>((block >> 24) & 0xFF);
        s += static_cast<char>((block >> 16) & 0xFF);
        s += static_cast<char>((block >> 8) & 0xFF);
        s += static_cast<char>(block & 0xFF);
        Digest u = hmac_sha256(password, s);
        Digest t = u;
        for (uint32_t i = 1; i < iterations; ++i) {
            u = hmac_sha256(password, digest_bytes(u));
            for (size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        const size_t take = std::min(t.size(), out_len - out.size());
        out.append(reinterpret_cast<const char*>(t.data()), take);
        ++block;
    }
    return out;
}

bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::string to_hex(std::string_view bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += kHex[c >> 4];
        out += kHex[c & 15];
    }
    return out;
}

std::string to_hex(const Digest& d) {
    return to_hex(digest_bytes(d));
}

bool from_hex(std::string_view hex, std::string* out) {
    if (hex.size() % 2 != 0) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        result += static_cast<char>((hi << 4) | lo);
    }
    *out = std::move(result);
    return true;
}

std::string base64_encode(std::string_view bytes) {
    return b64_encode(bytes, kB64, true);
}

bool base64_decode(std::string_view text, std::string* out) {
    return b64_decode(text, false, out);
}

std::string base64url_encode(std::string_view bytes) {
    return b64_encode(bytes, kB64Url, false);
}

bool base64url_decode(std::string_view text, std::string* out) {
    return b64_decode(text, true, out);
}

std::string random_bytes(size_t n) {
    static thread_local std::random_device rd;
    std::string out;
    out.reserve(n);
    while (out.size() < n) {
        uint32_t v = rd();
        for (int i = 0; i < 4 && out.size() < n; ++i) {
            out += static_cast<char>(v & 0xFF);
            v >>= 8;
        }
    }
    return out;
}

std::string random_token(size_t n_bytes) {
    return base64url_encode(random_bytes(n_bytes));
}

}  // namespace tcrm::crypto
