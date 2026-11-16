// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "storage/database.hpp"

#include <filesystem>
#include <fstream>

#include "util/log.hpp"

namespace fs = std::filesystem;

namespace tcrm::storage {

json::Value value_to_storage(const FieldValue& v) {
    switch (v.kind()) {
        case ValueKind::Null: return json::Value();
        case ValueKind::Bool: return json::Value(v.as_bool());
        case ValueKind::Int: return json::Value(v.as_int());
        case ValueKind::Double: return json::Value(v.as_double());
        case ValueKind::String: return json::Value(v.as_string());
        case ValueKind::Date: {
            json::Value o = json::Value::object();
            o.set("$d", v.as_date());
            return o;
        }
        case ValueKind::DateTime: {
            json::Value o = json::Value::object();
            o.set("$t", v.as_datetime());
            return o;
        }
    }
    return json::Value();
}

FieldValue value_from_storage(const json::Value& v) {
    switch (v.type()) {
        case json::Value::Type::Null: return FieldValue::null();
        case json::Value::Type::Bool: return FieldValue::boolean(v.as_bool());
        case json::Value::Type::Number:
            return v.is_integer() ? FieldValue::integer(v.as_int()) : FieldValue::number(v.as_double());
        case json::Value::Type::String: return FieldValue::string(v.as_string());
        case json::Value::Type::Object:
            if (const json::Value* d = v.find("$d")) return FieldValue::date(d->as_int());
            if (const json::Value* t = v.find("$t")) return FieldValue::datetime(t->as_int());
            return FieldValue::null();
        case json::Value::Type::Array: return FieldValue::null();
    }
    return FieldValue::null();
}

json::Value record_to_json(const Record& r) {
    json::Value o = json::Value::object();
    o.set("id", r.id);
    o.set("object", r.object);
    o.set("version", static_cast<int64_t>(r.version));
    json::Value fields = json::Value::object();
    for (const auto& [name, value] : r.fields) {
        if (!value.is_null()) fields.set(name, value_to_storage(value));
    }
    o.set("fields", std::move(fields));
    return o;
}

Record record_from_json(const json::Value& v) {
    Record r;
    r.id = v.get_string("id");
    r.object = v.get_string("object");
    r.version = static_cast<uint64_t>(v.get_int("version", 0));
    const json::Value& fields = v.at("fields");
    if (fields.is_object()) {
        for (const auto& [name, value] : fields.as_object()) r.fields[name] = value_from_storage(value);
    }
    return r;
}

Database::Database(std::string dir) : dir_(std::move(dir)) {}

Database::~Database() {
    close();
}

std::string Database::snapshot_path() const {
    return (fs::path(dir_) / "snapshot.jsonl").string();
}

std::string Database::wal_path() const {
    return (fs::path(dir_) / "wal.log").string();
}

bool Database::open(std::string* error) {
    if (!persistent()) return true;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        if (error) *error = "cannot create " + dir_ + ": " + ec.message();
        return false;
    }
    replaying_ = true;
    if (fs::exists(snapshot_path())) {
        std::ifstream in(snapshot_path(), std::ios::binary);
        std::string line;
        size_t n = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            json::Value entry;
            std::string perr;
            if (!json::try_parse(line, &entry, &perr)) {
                replaying_ = false;
                if (error) *error = "corrupt snapshot " + snapshot_path() + ": " + perr;
                return false;
            }
            apply(entry);
            ++n;
        }
        log::debug("storage", dir_, ": loaded ", n, " snapshot entries");
    }
    Wal::ReplayStats stats;
    if (!Wal::replay(wal_path(), [this](const json::Value& e) { apply(e); }, &stats, error)) {
        replaying_ = false;
        return false;
    }
    replaying_ = false;
    if (stats.applied > 0 || stats.skipped > 0) {
        log::info("storage", dir_, ": replayed ", stats.applied, " log entries (", stats.skipped, " skipped)");
    }
    return wal_.open(wal_path(), error);
}

void Database::close() {
    wal_.close();
}

Table& Database::table(const std::string& object) {
    auto it = tables_.find(object);
    if (it == tables_.end()) it = tables_.emplace(object, Table(object)).first;
    return it->second;
}

const Table* Database::find_table(const std::string& object) const {
    auto it = tables_.find(object);
    return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> Database::table_names() const {
    std::vector<std::string> out;
    for (const auto& [name, t] : tables_) out.push_back(name);
    return out;
}

void Database::apply(const json::Value& entry) {
    const std::string op = entry.get_string("op");
    if (op == "put") {
        Record r = record_from_json(entry.at("record"));
        table(r.object).put(std::move(r));
    } else if (op == "erase") {
        auto it = tables_.find(entry.get_string("object"));
        if (it != tables_.end()) it->second.erase(entry.get_string("id"));
    } else if (op == "meta") {
        meta_[entry.get_string("key")] = entry.at("value");
    } else if (op == "unmeta") {
        meta_.erase(entry.get_string("key"));
    }
}

void Database::put(const Record& r) {
    table(r.object).put(r);
    if (!persistent() || replaying_) return;
    json::Value e = json::Value::object();
    e.set("op", "put");
    e.set("record", record_to_json(r));
    if (!wal_.append(e)) log::error("storage", dir_, ": log append failed");
}

void Database::erase(const std::string& object, const std::string& id) {
    auto it = tables_.find(object);
    if (it != tables_.end()) it->second.erase(id);
    if (!persistent() || replaying_) return;
    json::Value e = json::Value::object();
    e.set("op", "erase");
    e.set("object", object);
    e.set("id", id);
    if (!wal_.append(e)) log::error("storage", dir_, ": log append failed");
}

void Database::put_meta(const std::string& key, const json::Value& value) {
    meta_[key] = value;
    if (!persistent() || replaying_) return;
    json::Value e = json::Value::object();
    e.set("op", "meta");
    e.set("key", key);
    e.set("value", value);
    if (!wal_.append(e)) log::error("storage", dir_, ": log append failed");
}

void Database::erase_meta(const std::string& key) {
    meta_.erase(key);
    if (!persistent() || replaying_) return;
    json::Value e = json::Value::object();
    e.set("op", "unmeta");
    e.set("key", key);
    wal_.append(e);
}

const json::Value* Database::meta(const std::string& key) const {
    auto it = meta_.find(key);
    return it == meta_.end() ? nullptr : &it->second;
}

bool Database::snapshot(std::string* error) {
    if (!persistent()) return true;
    std::ofstream out(snapshot_path(), std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + snapshot_path();
        return false;
    }
    for (const auto& [key, value] : meta_) {
        json::Value e = json::Value::object();
        e.set("op", "meta");
        e.set("key", key);
        e.set("value", value);
        out << e.dump() << '\n';
    }
    for (const auto& [name, t] : tables_) {
        for (const auto& [id, r] : t.rows()) {
            json::Value e = json::Value::object();
            e.set("op", "put");
            e.set("record", record_to_json(r));
            out << e.dump() << '\n';
        }
    }
    out.close();
    if (!out) {
        if (error) *error = "short write to " + snapshot_path();
        return false;
    }
    return wal_.reset(error);
}

uint64_t Database::approximate_bytes() const {
    uint64_t total = 0;
    for (const auto& [name, t] : tables_) {
        for (const auto& [id, r] : t.rows()) {
            total += 64;
            for (const auto& [fname, v] : r.fields) {
                total += fname.size() + 16;
                if (v.kind() == ValueKind::String) total += v.as_string().size();
            }
        }
    }
    return total;
}

}  // namespace tcrm::storage
