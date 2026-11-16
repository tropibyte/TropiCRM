// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/csv.hpp"

namespace tcrm::csv {

bool Reader::next(std::vector<std::string>* row) {
    row->clear();
    error_.clear();
    if (pos_ >= data_.size()) return false;
    row_line_ = line_;
    std::string field;
    bool in_quotes = false;
    bool field_was_quoted = false;
    while (pos_ < data_.size()) {
        const char c = data_[pos_];
        if (in_quotes) {
            if (c == '"') {
                if (pos_ + 1 < data_.size() && data_[pos_ + 1] == '"') {
                    field += '"';
                    pos_ += 2;
                    continue;
                }
                in_quotes = false;
                ++pos_;
                continue;
            }
            if (c == '\n') ++line_;
            field += c;
            ++pos_;
            continue;
        }
        if (c == '"') {
            if (!field.empty() || field_was_quoted) {
                error_ = "unexpected quote on line " + std::to_string(line_);
                return false;
            }
            in_quotes = true;
            field_was_quoted = true;
            ++pos_;
            continue;
        }
        if (c == delim_) {
            row->push_back(std::move(field));
            field.clear();
            field_was_quoted = false;
            ++pos_;
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (c == '\r' && pos_ + 1 < data_.size() && data_[pos_ + 1] == '\n') ++pos_;
            ++pos_;
            ++line_;
            row->push_back(std::move(field));
            return true;
        }
        if (field_was_quoted) {
            error_ = "text after closing quote on line " + std::to_string(line_);
            return false;
        }
        field += c;
        ++pos_;
    }
    if (in_quotes) {
        error_ = "unterminated quoted field starting on line " + std::to_string(row_line_);
        return false;
    }
    row->push_back(std::move(field));
    return true;
}

std::string escape(std::string_view field, char delimiter) {
    bool needs_quotes = false;
    for (char c : field) {
        if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return std::string(field);
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string format_row(const std::vector<std::string>& fields, char delimiter) {
    std::string out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) out += delimiter;
        out += escape(fields[i], delimiter);
    }
    out += "\r\n";
    return out;
}

}  // namespace tcrm::csv
