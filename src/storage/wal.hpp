// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>

#include "json/json.hpp"

namespace tcrm::storage {

uint32_t crc32(std::string_view data);

/// An append-only log of JSON entries, one per line:
///   <crc32 hex> <length> <json>\n
class Wal {
public:
    Wal() = default;
    ~Wal() { close(); }

    bool open(const std::string& path, std::string* error);
    void close();
    bool is_open() const { return out_.is_open(); }

    bool append(const json::Value& entry);

    /// Empties the log after a snapshot has captured its contents.
    bool reset(std::string* error);

    uint64_t entries() const { return entries_; }

    struct ReplayStats {
        size_t applied = 0;
        size_t skipped = 0;
        bool truncated_tail = false;
    };

    /// Calls `fn` for every intact entry in order. A torn final line (a
    /// crash during append) is ignored.
    static bool replay(const std::string& path, const std::function<void(const json::Value&)>& fn, ReplayStats* stats,
                       std::string* error);

private:
    std::string path_;
    std::ofstream out_;
    uint64_t entries_ = 0;
};

}  // namespace tcrm::storage
