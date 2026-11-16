// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "model/record.hpp"
#include "storage/table.hpp"
#include "storage/wal.hpp"

namespace tcrm::storage {

/// JSON encoding of records that keeps value kinds: dates are {"$d": days},
/// date-times {"$t": ms}; everything else is plain JSON.
json::Value record_to_json(const Record& r);
Record record_from_json(const json::Value& v);
json::Value value_to_storage(const FieldValue& v);
FieldValue value_from_storage(const json::Value& v);

/// The persistent state of one org: record tables plus named metadata
/// documents (schema, users, settings, ...). Every change is appended to a
/// write-ahead log; snapshot() folds the log into a snapshot file.
///
/// Not thread-safe. The owning Org serializes access.
class Database {
public:
    /// `dir` empty means purely in memory (tests, the admin CLI's dry runs).
    explicit Database(std::string dir = {});
    ~Database();

    /// Loads the snapshot and replays the log.
    bool open(std::string* error);
    void close();

    bool persistent() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    Table& table(const std::string& object);
    const Table* find_table(const std::string& object) const;
    std::vector<std::string> table_names() const;

    /// Logged mutations.
    void put(const Record& r);
    void erase(const std::string& object, const std::string& id);
    void put_meta(const std::string& key, const json::Value& value);
    void erase_meta(const std::string& key);

    const json::Value* meta(const std::string& key) const;

    /// Writes a snapshot of everything and empties the log.
    bool snapshot(std::string* error);

    uint64_t log_entries() const { return wal_.entries(); }
    uint64_t approximate_bytes() const;

private:
    void apply(const json::Value& entry);
    std::string snapshot_path() const;
    std::string wal_path() const;

    std::string dir_;
    std::map<std::string, Table> tables_;
    std::map<std::string, json::Value> meta_;
    Wal wal_;
    bool replaying_ = false;
};

}  // namespace tcrm::storage
