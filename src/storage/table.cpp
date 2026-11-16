// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "storage/table.hpp"

namespace tcrm::storage {

void Index::insert(const FieldValue& key, const std::string& id) {
    if (key.is_null()) return;
    entries_.emplace(key, id);
}

void Index::erase(const FieldValue& key, const std::string& id) {
    auto [lo, hi] = entries_.equal_range(key);
    for (auto it = lo; it != hi; ++it) {
        if (it->second == id) {
            entries_.erase(it);
            return;
        }
    }
}

std::vector<std::string> Index::find(const FieldValue& key) const {
    std::vector<std::string> out;
    auto [lo, hi] = entries_.equal_range(key);
    for (auto it = lo; it != hi; ++it) out.push_back(it->second);
    return out;
}

std::vector<std::string> Index::range(const FieldValue* lo, bool lo_inclusive, const FieldValue* hi,
                                      bool hi_inclusive) const {
    std::vector<std::string> out;
    auto it = lo == nullptr ? entries_.begin() : (lo_inclusive ? entries_.lower_bound(*lo) : entries_.upper_bound(*lo));
    for (; it != entries_.end(); ++it) {
        if (hi != nullptr) {
            const int c = it->first.compare(*hi);
            if (c > 0 || (c == 0 && !hi_inclusive)) break;
        }
        out.push_back(it->second);
    }
    return out;
}

bool Index::taken_by_other(const FieldValue& key, const std::string& id) const {
    if (key.is_null()) return false;
    auto [lo, hi] = entries_.equal_range(key);
    for (auto it = lo; it != hi; ++it) {
        if (it->second != id) return true;
    }
    return false;
}

const Record* Table::get(const std::string& id) const {
    auto it = rows_.find(id);
    return it == rows_.end() ? nullptr : &it->second;
}

void Table::put(Record record) {
    auto it = rows_.find(record.id);
    if (it == rows_.end()) {
        index_add(record);
        const std::string id = record.id;
        rows_.emplace(id, std::move(record));
        return;
    }
    index_update(it->second, record);
    it->second = std::move(record);
}

bool Table::erase(const std::string& id) {
    auto it = rows_.find(id);
    if (it == rows_.end()) return false;
    index_remove(it->second);
    rows_.erase(it);
    return true;
}

void Table::ensure_index(const std::string& field, bool unique) {
    if (indexes_.count(field) != 0) return;
    Index idx(field, unique);
    for (const auto& [id, r] : rows_) idx.insert(r.get(field), id);
    indexes_.emplace(field, std::move(idx));
}

void Table::drop_index(const std::string& field) {
    indexes_.erase(field);
}

const Index* Table::index(const std::string& field) const {
    auto it = indexes_.find(field);
    return it == indexes_.end() ? nullptr : &it->second;
}

std::vector<std::string> Table::indexed_fields() const {
    std::vector<std::string> out;
    for (const auto& [name, idx] : indexes_) out.push_back(name);
    return out;
}

void Table::index_add(const Record& r) {
    for (auto& [field, idx] : indexes_) idx.insert(r.get(field), r.id);
}

void Table::index_remove(const Record& r) {
    for (auto& [field, idx] : indexes_) idx.erase(r.get(field), r.id);
}

void Table::index_update(const Record& before, const Record& after) {
    for (auto& [field, idx] : indexes_) {
        const FieldValue& old_value = before.get(field);
        const FieldValue& new_value = after.get(field);
        if (new_value.is_null() || old_value == new_value) continue;
        idx.erase(old_value, before.id);
        idx.insert(new_value, after.id);
    }
}

}  // namespace tcrm::storage
