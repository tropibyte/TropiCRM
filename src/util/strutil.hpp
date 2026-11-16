// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tcrm::str {

/// Whitespace is space, tab, CR and LF.
std::string_view trim(std::string_view s);
std::string_view ltrim(std::string_view s);
std::string_view rtrim(std::string_view s);

std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);

/// ASCII case-insensitive comparison.
bool iequals(std::string_view a, std::string_view b);
bool istarts_with(std::string_view s, std::string_view prefix);
bool iends_with(std::string_view s, std::string_view suffix);
bool contains_ci(std::string_view haystack, std::string_view needle);

/// Three-way case-insensitive compare, for sorting names.
int icompare(std::string_view a, std::string_view b);

/// Splits on every occurrence of `sep`. Empty pieces are kept.
std::vector<std::string> split(std::string_view s, char sep);

/// Splits on `sep`, trims each piece and drops empty pieces.
std::vector<std::string> split_trimmed(std::string_view s, char sep);

std::string join(const std::vector<std::string>& parts, std::string_view sep);

std::string replace_all(std::string s, std::string_view from, std::string_view to);

/// Strict integer parse: the whole string must be an optionally signed
/// decimal integer that fits in 64 bits.
bool parse_int64(std::string_view s, int64_t* out);

/// Strict floating-point parse in the "C" locale.
bool parse_double(std::string_view s, double* out);

/// Formats with at most `max_decimals` digits after the point and no
/// trailing zeros ("12.5", "3", "-0.25").
std::string format_double(double v, int max_decimals = 10);

/// Left-pads a non-negative integer with zeros to `width` digits.
std::string zero_pad(int64_t v, int width);

/// Letters, digits and underscores, not starting with a digit.
bool is_identifier(std::string_view s);

/// Shortens `s` to at most `max_len` bytes without splitting a UTF-8
/// sequence, appending "..." when something was cut.
std::string ellipsize(std::string_view s, size_t max_len);

/// Human-readable byte counts: "512 B", "1.5 KB", "20 MB".
std::string format_bytes(uint64_t bytes);

}  // namespace tcrm::str
