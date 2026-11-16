// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "storage/wal.hpp"

#include <array>
#include <cstdio>
#include <filesystem>

#include "util/log.hpp"
#include "util/strutil.hpp"

namespace tcrm::storage {
namespace {

std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}

}  // namespace

uint32_t crc32(std::string_view data) {
    static const std::array<uint32_t, 256> kTable = make_crc_table();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = kTable[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool Wal::open(const std::string& path, std::string* error) {
    close();
    path_ = path;
    out_.open(path, std::ios::binary | std::ios::app);
    if (!out_.is_open()) {
        if (error) *error = "cannot open log " + path;
        return false;
    }
    return true;
}

void Wal::close() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

bool Wal::append(const json::Value& entry) {
    if (!out_.is_open()) return false;
    const std::string body = entry.dump();
    char head[32];
    std::snprintf(head, sizeof head, "%08x %zu ", crc32(body), body.size());
    out_ << head << body << '\n';
    out_.flush();
    ++entries_;
    return static_cast<bool>(out_);
}

bool Wal::reset(std::string* error) {
    out_.close();
    out_.open(path_, std::ios::binary | std::ios::trunc);
    if (!out_.is_open()) {
        if (error) *error = "cannot truncate log " + path_;
        return false;
    }
    entries_ = 0;
    return true;
}

bool Wal::replay(const std::string& path, const std::function<void(const json::Value&)>& fn, ReplayStats* stats,
                 std::string* error) {
    ReplayStats local;
    if (!std::filesystem::exists(path)) {
        if (stats) *stats = local;
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot read log " + path;
        return false;
    }
    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;
        const bool last = in.peek() == std::char_traits<char>::eof();
        const size_t sp1 = line.find(' ');
        const size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
        bool ok = sp2 != std::string::npos;
        uint32_t want_crc = 0;
        int64_t want_len = 0;
        std::string_view body;
        if (ok) {
            ok = std::sscanf(line.substr(0, sp1).c_str(), "%x", &want_crc) == 1 &&
                 str::parse_int64(std::string_view(line).substr(sp1 + 1, sp2 - sp1 - 1), &want_len);
            body = std::string_view(line).substr(sp2 + 1);
            ok = ok && static_cast<int64_t>(body.size()) == want_len && crc32(body) == want_crc;
        }
        if (!ok) {
            if (last) {
                local.truncated_tail = true;
                log::warn("wal", path, ": ignoring torn entry at line ", line_no);
                break;
            }
            ++local.skipped;
            log::warn("wal", path, ": skipping corrupt entry at line ", line_no);
            continue;
        }
        json::Value entry;
        std::string perr;
        if (!json::try_parse(body, &entry, &perr)) {
            ++local.skipped;
            continue;
        }
        fn(entry);
        ++local.applied;
    }
    if (stats) *stats = local;
    return true;
}

}  // namespace tcrm::storage
