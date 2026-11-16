// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "model/record.hpp"

namespace tcrm::storage {

/// An ordered secondary index from field value to record ids.
class Index {
public:
    Index() = default;
    Index(std::string field, bool unique) : field_(std::move(field)), unique_(unique) {}

    const std::string& field() const { return field_; }
    bool unique() const { return unique_; }

    void insert(const FieldValue& key, const std::string& id);
    void erase(const FieldValue& key, const std::string& id);

    std::vector<std::string> find(const FieldValue& key) const;

    /// Ids whose key lies in the given bounds; a null bound is open.
    std::vector<std::string> range(const FieldValue* lo, bool lo_inclusive, const FieldValue* hi,
                                   bool hi_inclusive) const;

    /// True if some record other than `id` already has `key`.
    bool taken_by_other(const FieldValue& key, const std::string& id) const;

    size_t size() const { return entries_.size(); }

private:
    std::string field_;
    bool unique_ = false;
    std::multimap<FieldValue, std::string> entries_;
};

/// All rows of one object, keyed by id, plus its secondary indexes.
class Table {
public:
    explicit Table(std::string object = {}) : object_(std::move(object)) {}

    const std::string& object() const { return object_; }

    const Record* get(const std::string& id) const;

    /// Inserts or replaces a record and maintains the indexes.
    void put(Record record);

    bool erase(const std::string& id);

    size_t size() const { return rows_.size(); }

    const std::unordered_map<std::string, Record>& rows() const { return rows_; }
    std::unordered_map<std::string, Record>& mutable_rows() { return rows_; }

    /// Creates an index (and fills it) if it doesn't exist yet.
    void ensure_index(const std::string& field, bool unique);
    void drop_index(const std::string& field);
    const Index* index(const std::string& field) const;
    std::vector<std::string> indexed_fields() const;

private:
    void index_add(const Record& r);
    void index_remove(const Record& r);
    void index_update(const Record& before, const Record& after);

    std::string object_;
    std::unordered_map<std::string, Record> rows_;
    std::map<std::string, Index> indexes_;
};

}  // namespace tcrm::storage
