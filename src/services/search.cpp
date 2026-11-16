// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "services/search.hpp"

#include <algorithm>
#include <cmath>
#include <shared_mutex>

#include "engine/records.hpp"
#include "model/errors.hpp"
#include "util/strutil.hpp"

namespace tcrm::services {
namespace {

bool is_word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
}

std::vector<std::string> record_terms(const meta::ObjectDef& obj, const Record& r) {
    std::vector<std::string> terms;
    for (const meta::FieldDef& f : obj.fields) {
        if (!f.searchable && !f.name_field) continue;
        const std::string text = r.text(f.name);
        if (text.empty()) continue;
        for (std::string& t : tokenize(text)) terms.push_back(std::move(t));
    }
    return terms;
}

}  // namespace

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    const std::string lowered = str::to_lower(text);
    std::string cur;
    for (char ch : lowered) {
        const auto c = static_cast<unsigned char>(ch);
        if (is_word_char(c)) {
            cur += ch;
        } else {
            if (cur.size() >= 2) out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() >= 2) out.push_back(cur);
    return out;
}

void SearchIndex::index(const std::string& object, const std::string& id, const std::vector<std::string>& terms) {
    remove(id);
    for (const std::string& t : terms) {
        Posting& p = postings_[t][id];
        p.object = object;
        ++p.frequency;
    }
    doc_terms_[id] = terms;
    doc_length_[id] = terms.size();
}

void SearchIndex::remove(const std::string& id) {
    auto it = doc_terms_.find(id);
    if (it == doc_terms_.end()) return;
    for (const std::string& t : it->second) {
        auto p = postings_.find(t);
        if (p == postings_.end()) continue;
        p->second.erase(id);
        if (p->second.empty()) postings_.erase(p);
    }
    doc_terms_.erase(it);
    doc_length_.erase(id);
}

std::vector<SearchIndex::Hit> SearchIndex::search(const std::vector<std::string>& terms,
                                                  const std::set<std::string>& objects, size_t limit) const {
    std::vector<Hit> out;
    if (terms.empty()) return out;
    const double n_docs = static_cast<double>(std::max<size_t>(1, doc_terms_.size()));
    std::unordered_map<std::string, Hit> scored;
    std::unordered_map<std::string, size_t> matched_terms;
    for (size_t ti = 0; ti < terms.size(); ++ti) {
        const bool prefix = ti + 1 == terms.size();
        auto begin = postings_.lower_bound(terms[ti]);
        std::set<std::string> seen_this_term;
        for (auto it = begin; it != postings_.end(); ++it) {
            const bool exact = it->first == terms[ti];
            if (!exact && !(prefix && str::istarts_with(it->first, terms[ti]))) break;
            const double idf = std::log(1.0 + n_docs / static_cast<double>(it->second.size()));
            for (const auto& [id, posting] : it->second) {
                if (!objects.empty() && objects.count(posting.object) == 0) continue;
                Hit& h = scored[id];
                h.id = id;
                h.object = posting.object;
                const double len = static_cast<double>(doc_length_.count(id) ? doc_length_.at(id) : 1);
                h.score += idf * static_cast<double>(posting.frequency) / (1.0 + len / 20.0) * (exact ? 1.0 : 0.6);
                if (seen_this_term.insert(id).second) ++matched_terms[id];
            }
        }
    }
    for (auto& [id, h] : scored) {
        if (matched_terms[id] == terms.size()) out.push_back(std::move(h));
    }
    std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) {
        return a.score != b.score ? a.score > b.score : a.id < b.id;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

size_t SearchIndex::term_count() const {
    return postings_.size();
}

SearchIndex& SearchService::index_for(const std::string& org_id) {
    return indexes_[org_id];
}

void SearchService::saved(engine::Org& org, const meta::ObjectDef& obj, const Record* before, const Record& after) {
    (void)before;
    if (!obj.searchable) return;
    std::vector<std::string> terms = record_terms(obj, after);
    std::lock_guard<std::mutex> lock(mu_);
    index_for(org.id()).index(obj.name, after.id, terms);
}

void SearchService::rebuild(engine::Org& org) {
    std::shared_lock<std::shared_mutex> org_lock(org.mutex());
    std::lock_guard<std::mutex> lock(mu_);
    SearchIndex& idx = index_for(org.id());
    idx = SearchIndex();
    for (const meta::ObjectDef* obj : org.schema.objects()) {
        if (!obj->searchable) continue;
        const storage::Table* t = org.db.find_table(obj->name);
        if (t == nullptr) continue;
        for (const auto& [id, r] : t->rows()) {
            if (!r.is_deleted()) idx.index(obj->name, id, record_terms(*obj, r));
        }
    }
}

json::Value SearchService::search(const engine::Principal& who, const Options& options) {
    const std::vector<std::string> terms = tokenize(options.query);
    if (terms.empty()) {
        throw errors::bad_request("MALFORMED_SEARCH", "Search term must be longer than one character");
    }
    engine::Org& org = *who.org;
    std::shared_lock<std::shared_mutex> org_lock(org.mutex());
    engine::AccessChecker access(org, who);

    std::set<std::string> objects;
    for (const std::string& name : options.objects) {
        const meta::ObjectDef* obj = org.schema.object(name);
        if (obj == nullptr) throw errors::bad_request("INVALID_TYPE", "sObject type '" + name + "' is not supported.");
        objects.insert(obj->name);
    }
    std::vector<SearchIndex::Hit> hits;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Over-fetch: some hits will be filtered out by sharing.
        hits = index_for(org.id()).search(terms, objects, options.limit * 4 + 20);
    }
    json::Value records = json::Value::array();
    for (const SearchIndex::Hit& h : hits) {
        if (records.size() >= options.limit) break;
        const meta::ObjectDef* obj = org.schema.object(h.object);
        if (obj == nullptr || !access.object_allowed(*obj, engine::ObjectOp::Read)) continue;
        const storage::Table* t = org.db.find_table(obj->name);
        const Record* r = t ? t->get(h.id) : nullptr;
        if (r == nullptr || !access.can_read(*obj, *r)) continue;
        json::Value row = json::Value::object();
        row.set("attributes", engine::record_attributes(obj->name, r->id));
        row.set("Id", r->id);
        const meta::FieldDef* name_field = obj->field(obj->name_field);
        if (name_field != nullptr && access.field_readable(*obj, *name_field)) {
            row.set(name_field->name, engine::record_name(*obj, *r));
        }
        row.set("score", std::round(h.score * 1000.0) / 1000.0);
        records.push_back(std::move(row));
    }
    json::Value out = json::Value::object();
    out.set("searchRecords", std::move(records));
    return out;
}

}  // namespace tcrm::services
