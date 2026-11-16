// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/access.hpp"
#include "engine/org.hpp"
#include "json/json.hpp"
#include "query/ast.hpp"

namespace tcrm::query {

struct ExecOptions {
    int64_t batch_size = 2000;
    bool include_deleted = false;  // queryAll
};

struct QueryResult {
    int64_t total_size = 0;
    bool done = true;
    std::string next_cursor;
    json::Value records = json::Value::array();

    json::Value to_json() const;
};

/// Server-side cursors for results larger than one batch.
class CursorStore {
public:
    std::string open(const std::string& org_id, const std::string& user_id, std::deque<json::Value> rows,
                     int64_t total, int64_t delivered);

    /// Fills the next batch of `cursor` into `out`. Returns false for an
    /// unknown cursor.
    bool next(const std::string& cursor, int64_t batch_size, QueryResult* out);

    size_t open_cursors() const;

private:
    struct Cursor {
        std::string org_id;
        std::string user_id;
        std::deque<json::Value> rows;
        int64_t total = 0;
        int64_t delivered = 0;
        int64_t created_at = 0;
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, Cursor> cursors_;
    uint64_t next_id_ = 1;
};

/// Runs a parsed query for one principal. The caller holds the org's lock
/// (shared is enough).
class Executor {
public:
    Executor(const engine::Org& org, const engine::Principal& who);

    QueryResult run(const Query& q, const ExecOptions& options, CursorStore* cursors);

    /// The ids of matching records, after filtering and sharing, in query
    /// order. LIMIT/OFFSET are applied.
    std::vector<std::string> matching_ids(const Query& q, bool include_deleted);

private:
    struct Resolved {
        std::vector<const meta::FieldDef*> lookups;
        const meta::FieldDef* field = nullptr;
        const meta::ObjectDef* object = nullptr;  // owner of `field`
        bool polymorphic = false;
        std::string polymorphic_field;  // "Id" or "Name"
    };

    const Resolved& resolve(const meta::ObjectDef& base, const FieldPath& path, bool check_fls);
    FieldValue value_of(const Record& r, const Resolved& rp) const;
    void check_operand_types(const Resolved& rp, const Condition& c) const;
    void bind_condition(const meta::ObjectDef& base, const Condition& c, bool having);

    bool eval(const meta::ObjectDef& base, const Condition& c, const Record& r);
    bool compare(const FieldValue& lhs, const std::string& op, const Operand& rhs, const meta::FieldDef* field) const;

    std::vector<const Record*> candidates(const meta::ObjectDef& obj, const Query& q);
    std::vector<const Record*> filter(const meta::ObjectDef& obj, const Query& q, bool include_deleted);
    void sort(const meta::ObjectDef& obj, const std::vector<OrderItem>& order, std::vector<const Record*>* rows);

    json::Value build_row(const meta::ObjectDef& obj, const Record& r, const Query& q);
    json::Value child_rows(const meta::ObjectDef& parent, const Record& r, const Query& sub);
    QueryResult run_aggregate(const meta::ObjectDef& obj, const Query& q, bool include_deleted);

    std::pair<int64_t, int64_t> date_range(const std::string& literal) const;

    const engine::Org& org_;
    const engine::Principal& who_;
    engine::AccessChecker access_;
    std::map<std::string, Resolved> resolved_;  // by "Object|path|fls"
    std::vector<Record> user_rows_;
};

/// Parses and runs `tql` for `who`, taking the org's shared lock.
QueryResult execute(const std::string& tql, const engine::Principal& who, const ExecOptions& options,
                    CursorStore* cursors);

/// LIKE matching: % is any run of characters, _ is one character,
/// backslash escapes. Case-insensitive.
bool like_match(const std::string& text, const std::string& pattern);

}  // namespace tcrm::query
