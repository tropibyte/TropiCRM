// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/access.hpp"
#include "engine/record_service.hpp"
#include "json/json.hpp"

namespace tcrm::services {

/// Splits text into lowercase search terms (letters and digits, 2+ chars).
std::vector<std::string> tokenize(const std::string& text);

/// A per-org inverted index over searchable fields.
class SearchIndex {
public:
    void index(const std::string& object, const std::string& id, const std::vector<std::string>& terms);
    void remove(const std::string& id);

    struct Hit {
        std::string object;
        std::string id;
        double score = 0;
    };
    /// Records matching every term (prefix match on the last term).
    std::vector<Hit> search(const std::vector<std::string>& terms, const std::set<std::string>& objects,
                            size_t limit) const;

    size_t term_count() const;

private:
    struct Posting {
        std::string object;
        uint32_t frequency = 0;
    };
    std::map<std::string, std::unordered_map<std::string, Posting>> postings_;  // term -> id -> posting
    std::unordered_map<std::string, std::vector<std::string>> doc_terms_;      // id -> terms
    std::unordered_map<std::string, size_t> doc_length_;
};

/// Keeps one SearchIndex per org up to date and answers search requests.
class SearchService : public engine::RecordHooks {
public:
    void saved(engine::Org& org, const meta::ObjectDef& obj, const Record* before, const Record& after) override;

    /// Indexes everything already stored (at startup).
    void rebuild(engine::Org& org);

    struct Options {
        std::string query;
        std::set<std::string> objects;  // empty = all searchable objects
        size_t limit = 25;
    };

    /// Returns {"searchRecords": [...]} for the records the principal can see.
    json::Value search(const engine::Principal& who, const Options& options);

private:
    SearchIndex& index_for(const std::string& org_id);

    std::mutex mu_;
    std::map<std::string, SearchIndex> indexes_;
};

}  // namespace tcrm::services
