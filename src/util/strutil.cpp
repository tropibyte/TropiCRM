// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/strutil.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace tcrm::str {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::string_view ltrim(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    return s.substr(i);
}

std::string_view rtrim(std::string_view s) {
    size_t n = s.size();
    while (n > 0 && is_space(s[n - 1])) --n;
    return s.substr(0, n);
}

std::string_view trim(std::string_view s) {
    return rtrim(ltrim(s));
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(c));
    return out;
}

std::string to_upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(c));
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool iends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (iequals(haystack.substr(i, needle.size()), needle)) return true;
    }
    return false;
}

int icompare(std::string_view a, std::string_view b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const char x = ascii_lower(a[i]);
        const char y = ascii_lower(b[i]);
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::vector<std::string> split_trimmed(std::string_view s, char sep) {
    std::vector<std::string> out;
    for (const std::string& piece : split(s, sep)) {
        std::string_view t = trim(piece);
        if (!t.empty()) out.emplace_back(t);
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool parse_int64(std::string_view s, int64_t* out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    if (*first == '+') ++first;
    int64_t v = 0;
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || ptr != last) return false;
    *out = v;
    return true;
}

bool parse_double(std::string_view s, double* out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    if (*first == '+') ++first;
    double v = 0;
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || ptr != last) return false;
    if (!std::isfinite(v)) return false;
    *out = v;
    return true;
}

std::string format_double(double v, int max_decimals) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    if (v == std::floor(v) && std::fabs(v) < 9.007199254740992e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", max_decimals, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string zero_pad(int64_t v, int width) {
    std::string digits = std::to_string(v < 0 ? -v : v);
    if (static_cast<int>(digits.size()) < width) {
        digits.insert(0, static_cast<size_t>(width) - digits.size(), '0');
    }
    return v < 0 ? "-" + digits : digits;
}

bool is_identifier(std::string_view s) {
    if (s.empty()) return false;
    if (s[0] >= '0' && s[0] <= '9') return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string ellipsize(std::string_view s, size_t max_len) {
    if (s.size() <= max_len) return std::string(s);
    if (max_len <= 3) return std::string(s.substr(0, max_len));
    size_t cut = max_len - 3;
    // Back up to the start of a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return std::string(s.substr(0, cut)) + "...";
}

std::string format_bytes(uint64_t bytes) {
    static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 4) {
        v /= 1024.0;
        ++unit;
    }
    if (unit == 0) return std::to_string(bytes) + " B";
    return format_double(std::round(v * 10.0) / 10.0, 1) + " " + kUnits[unit];
}

}  // namespace tcrm::str
