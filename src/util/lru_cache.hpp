// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace tcrm {

/// A thread-safe least-recently-used cache. Values are returned by copy.
template <typename K, typename V, typename Hash = std::hash<K>>
class LruCache {
public:
    explicit LruCache(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            ++misses_;
            return std::nullopt;
        }
        order_.splice(order_.begin(), order_, it->second);
        ++hits_;
        return it->second->second;
    }

    void put(const K& key, V value) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->second = std::move(value);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        order_.emplace_front(key, std::move(value));
        map_[key] = order_.begin();
        while (map_.size() > capacity_) {
            map_.erase(order_.back().first);
            order_.pop_back();
        }
    }

    void erase(const K& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) return;
        order_.erase(it->second);
        map_.erase(it);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        map_.clear();
        order_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return map_.size();
    }

    size_t hits() const {
        std::lock_guard<std::mutex> lock(mu_);
        return hits_;
    }

    size_t misses() const {
        std::lock_guard<std::mutex> lock(mu_);
        return misses_;
    }

private:
    using Entry = std::pair<K, V>;
    size_t capacity_;
    std::list<Entry> order_;
    std::unordered_map<K, typename std::list<Entry>::iterator, Hash> map_;
    mutable std::mutex mu_;
    size_t hits_ = 0;
    size_t misses_ = 0;
};

}  // namespace tcrm
