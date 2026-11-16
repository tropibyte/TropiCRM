// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tcrm::csv {

/// Reads RFC 4180 CSV: comma-separated, double-quoted fields with "" as an
/// escaped quote, CRLF or LF line endings, newlines allowed inside quotes.
class Reader {
public:
    explicit Reader(std::string_view data, char delimiter = ',') : data_(data), delim_(delimiter) {}

    /// Reads the next row. Returns false at end of input or on error; check
    /// error() to tell them apart.
    bool next(std::vector<std::string>* row);

    const std::string& error() const { return error_; }

    /// 1-based line number where the last row returned started.
    size_t line() const { return row_line_; }

private:
    std::string_view data_;
    char delim_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t row_line_ = 0;
    std::string error_;
};

/// Quotes a field when it contains the delimiter, a quote or a line break.
std::string escape(std::string_view field, char delimiter = ',');

/// Escapes and joins one row, ending it with CRLF.
std::string format_row(const std::vector<std::string>& fields, char delimiter = ',');

}  // namespace tcrm::csv
