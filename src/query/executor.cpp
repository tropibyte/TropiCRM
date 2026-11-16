// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "query/executor.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <shared_mutex>

#include "engine/records.hpp"
#include "meta/codec.hpp"
#include "model/errors.hpp"
#include "util/id.hpp"
#include "util/strutil.hpp"
#include "util/time.hpp"

namespace tcrm::query {

using meta::FieldType;

namespace {

constexpr int64_t kMaxOffset = 2000;
constexpr size_t kMaxRelationshipDepth = 5;

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool like_at(const char* s, const char* p) {
    if (*p == '\0') return *s == '\0';
    if (*p == '%') return like_at(s, p + 1) || (*s != '\0' && like_at(s + 1, p));
    if (*s == '\0') return false;
    if (*p == '\\' && p[1] != '\0') return lower(*s) == lower(p[1]) && like_at(s + 1, p + 2);
    if (*p == '_' || lower(*s) == lower(*p)) return like_at(s + 1, p + 1);
    return false;
}

bool is_text_like(FieldType t) {
    return meta::is_text_type(t) || t == FieldType::Id;
}

std::string type_word(FieldType t) {
    if (meta::is_numeric_type(t)) return "double";
    if (t == FieldType::Checkbox) return "boolean";
    if (t == FieldType::Date) return "date";
    if (t == FieldType::DateTime) return "datetime";
    return "string";
}

int compare_for_sort(const FieldValue& a, const FieldValue& b) {
    if (a.kind() == ValueKind::String && b.kind() == ValueKind::String) {
        const int c = str::icompare(a.as_string(), b.as_string());
        if (c != 0) return c;
    }
    return a.compare(b);
}

std::string order_key(const OrderItem& o) {
    if (o.aggregate.empty()) return str::to_lower(o.path.text());
    return o.aggregate + "(" + str::to_lower(o.path.text()) + ")";
}

std::string condition_key(const Condition& c) {
    return c.aggregate + "(" + str::to_lower(c.field.text()) + ")";
}

}  // namespace

bool like_match(const std::string& text, const std::string& pattern) {
    return like_at(text.c_str(), pattern.c_str());
}

// ---- QueryResult / CursorStore -------------------------------------------

json::Value QueryResult::to_json() const {
    json::Value v = json::Value::object();
    v.set("totalSize", total_size);
    v.set("done", done);
    if (!done) v.set("nextRecordsUrl", std::string(engine::kApiBase) + "/query/" + next_cursor);
    v.set("records", records);
    return v;
}

std::string CursorStore::open(const std::string& org_id, const std::string& user_id, std::deque<json::Value> rows,
                              int64_t total, int64_t delivered) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string id = "01g" + ids::encode_base62(next_id_++, 12) + "-" + std::to_string(delivered);
    Cursor c;
    c.org_id = org_id;
    c.user_id = user_id;
    c.rows = std::move(rows);
    c.total = total;
    c.delivered = delivered;
    c.created_at = timeutil::now_ms();
    cursors_.emplace(id, std::move(c));
    return id;
}

bool CursorStore::next(const std::string& cursor, int64_t batch_size, QueryResult* out) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cursors_.find(cursor);
    if (it == cursors_.end()) return false;
    Cursor c = std::move(it->second);
    cursors_.erase(it);
    QueryResult r;
    r.total_size = c.total;
    int64_t n = 0;
    while (!c.rows.empty() && n < batch_size) {
        r.records.push_back(std::move(c.rows.front()));
        c.rows.pop_front();
        ++n;
    }
    c.delivered += n;
    if (c.rows.empty()) {
        r.done = true;
    } else {
        r.done = false;
        const std::string base = cursor.substr(0, cursor.find('-'));
        r.next_cursor = base + "-" + std::to_string(c.delivered);
        cursors_.emplace(r.next_cursor, std::move(c));
    }
    *out = std::move(r);
    return true;
}

size_t CursorStore::open_cursors() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cursors_.size();
}

// ---- Executor -------------------------------------------------------------

Executor::Executor(const engine::Org& org, const engine::Principal& who) : org_(org), who_(who), access_(org, who) {}

const Executor::Resolved& Executor::resolve(const meta::ObjectDef& base, const FieldPath& path, bool check_fls) {
    const std::string key = base.name + "|" + str::to_lower(path.text()) + (check_fls ? "|1" : "|0");
    auto it = resolved_.find(key);
    if (it != resolved_.end()) return it->second;

    if (path.parts.size() > kMaxRelationshipDepth + 1) {
        throw errors::malformed_query("relationships can be at most " + std::to_string(kMaxRelationshipDepth) +
                                      " levels deep: " + path.text());
    }
    Resolved rp;
    const meta::ObjectDef* cur = &base;
    for (size_t i = 0; i + 1 < path.parts.size(); ++i) {
        const meta::FieldDef* link = cur->relationship_field(path.parts[i]);
        if (link == nullptr) {
            throw errors::bad_request("INVALID_FIELD", "Didn't understand relationship '" + path.parts[i] +
                                                           "' in field path '" + path.text() + "'");
        }
        if (check_fls && !access_.field_readable(*cur, *link)) {
            throw errors::bad_request("INVALID_FIELD", "No such column '" + link->name + "' on entity '" + cur->name + "'");
        }
        rp.lookups.push_back(link);
        if (link->reference_to.find(',') != std::string::npos || link->reference_to == "*") {
            if (i + 2 != path.parts.size() ||
                !(str::iequals(path.parts.back(), "Id") || str::iequals(path.parts.back(), "Name"))) {
                throw errors::bad_request("INVALID_FIELD", "Only Id and Name can be selected through polymorphic "
                                                           "relationship '" + path.parts[i] + "'");
            }
            rp.polymorphic = true;
            rp.polymorphic_field = str::iequals(path.parts.back(), "Id") ? "Id" : "Name";
            return resolved_.emplace(key, std::move(rp)).first->second;
        }
        cur = org_.schema.object(link->reference_to);
        if (cur == nullptr) throw errors::internal("relationship target missing: " + link->reference_to);
    }
    const meta::FieldDef* f = cur->field(path.last());
    if (f == nullptr || (check_fls && !access_.field_readable(*cur, *f))) {
        throw errors::bad_request("INVALID_FIELD", "No such column '" + path.last() + "' on entity '" + cur->name +
                                                       "'. If you are attempting to use a custom field, be sure to "
                                                       "append the '__c' after the custom field name.");
    }
    rp.field = f;
    rp.object = cur;
    return resolved_.emplace(key, std::move(rp)).first->second;
}

FieldValue Executor::value_of(const Record& r, const Resolved& rp) const {
    if (rp.lookups.empty()) return r.get(rp.field->name);
    const Record* cur = &r;
    std::vector<Record> chain;
    chain.reserve(rp.lookups.size());
    for (const meta::FieldDef* link : rp.lookups) {
        const FieldValue& v = cur->get(link->name);
        if (v.kind() != ValueKind::String) return FieldValue::null();
        if (rp.polymorphic && link == rp.lookups.back() && rp.polymorphic_field == "Id") return v;
        const meta::ObjectDef* parent_obj = nullptr;
        std::optional<Record> parent = engine::load_record(org_, v.as_string(), &parent_obj);
        if (!parent) return FieldValue::null();
        chain.push_back(std::move(*parent));
        cur = &chain.back();
        if (rp.polymorphic && link == rp.lookups.back()) {
            return FieldValue::string(engine::record_name(*parent_obj, *cur));
        }
    }
    return cur->get(rp.field->name);
}

std::pair<int64_t, int64_t> Executor::date_range(const std::string& literal) const {
    const timeutil::TimeZone tz = org_.tz();
    const int64_t today = tz.local_date(timeutil::now_ms());
    const timeutil::Civil c = timeutil::civil_from_days(today);
    const int dow = timeutil::day_of_week(today);
    const int64_t week_start = today - dow;
    const int64_t month_start = timeutil::days_from_civil(c.year, c.month, 1);
    const int quarter_first_month = ((c.month - 1) / 3) * 3 + 1;
    const int64_t quarter_start = timeutil::days_from_civil(c.year, quarter_first_month, 1);
    const int64_t year_start = timeutil::days_from_civil(c.year, 1, 1);

    if (literal == "TODAY") return {today, today + 1};
    if (literal == "YESTERDAY") return {today - 1, today};
    if (literal == "TOMORROW") return {today + 1, today + 2};
    if (literal == "THIS_WEEK") return {week_start, week_start + 7};
    if (literal == "LAST_WEEK") return {week_start - 7, week_start};
    if (literal == "NEXT_WEEK") return {week_start + 7, week_start + 14};
    if (literal == "THIS_MONTH") return {month_start, timeutil::add_months(month_start, 1)};
    if (literal == "LAST_MONTH") return {timeutil::add_months(month_start, -1), month_start};
    if (literal == "NEXT_MONTH") return {timeutil::add_months(month_start, 1), timeutil::add_months(month_start, 2)};
    if (literal == "LAST_90_DAYS") return {today - 90, today + 1};
    if (literal == "NEXT_90_DAYS") return {today, today + 91};
    if (literal == "THIS_QUARTER") return {quarter_start, timeutil::add_months(quarter_start, 3)};
    if (literal == "LAST_QUARTER") return {timeutil::add_months(quarter_start, -3), quarter_start};
    if (literal == "NEXT_QUARTER") return {timeutil::add_months(quarter_start, 3), timeutil::add_months(quarter_start, 6)};
    if (literal == "THIS_YEAR") return {year_start, timeutil::add_months(year_start, 12)};
    if (literal == "LAST_YEAR") return {timeutil::add_months(year_start, -12), year_start};
    if (literal == "NEXT_YEAR") return {timeutil::add_months(year_start, 12), timeutil::add_months(year_start, 24)};

    const size_t colon = literal.find(':');
    int64_t n = 0;
    str::parse_int64(literal.substr(colon + 1), &n);
    const std::string kw = literal.substr(0, colon);
    if (kw == "LAST_N_DAYS") return {today - n, today + 1};
    if (kw == "NEXT_N_DAYS") return {today, today + n + 1};
    if (kw == "N_DAYS_AGO") return {today - n, today - n + 1};
    if (kw == "LAST_N_MONTHS") return {timeutil::add_months(month_start, static_cast<int>(-n)), month_start};
    if (kw == "NEXT_N_MONTHS") {
        return {timeutil::add_months(month_start, 1), timeutil::add_months(month_start, static_cast<int>(n + 1))};
    }
    throw errors::malformed_query("unknown date literal " + literal);
}

void Executor::check_operand_types(const Resolved& rp, const Condition& c) const {
    const FieldType t = rp.polymorphic ? FieldType::Text : rp.field->value_type();
    const std::string fname = rp.polymorphic ? rp.polymorphic_field : rp.field->name;
    for (const Operand& o : c.values) {
        if (o.kind == Operand::Kind::DateRange) {
            if (t != FieldType::Date && t != FieldType::DateTime) {
                throw errors::malformed_query("date literal " + o.literal + " can't be used with field " + fname);
            }
            continue;
        }
        if (o.value.is_null()) continue;
        bool ok = true;
        if (meta::is_numeric_type(t)) ok = o.value.is_numeric();
        else if (t == FieldType::Checkbox) ok = o.value.kind() == ValueKind::Bool;
        else if (t == FieldType::Date) ok = o.value.kind() == ValueKind::Date;
        else if (t == FieldType::DateTime) ok = o.value.kind() == ValueKind::DateTime || o.value.kind() == ValueKind::Date;
        else if (is_text_like(t)) ok = o.value.kind() == ValueKind::String;
        if (!ok) {
            throw errors::malformed_query("value of filter criterion for field '" + fname + "' must be of type " +
                                          type_word(t) + (o.value.kind() == ValueKind::String
                                                              ? " and should not be enclosed in quotes"
                                                              : ""));
        }
    }
}

void Executor::bind_condition(const meta::ObjectDef& base, const Condition& c, bool having) {
    switch (c.kind) {
        case Condition::Kind::And:
        case Condition::Kind::Or:
        case Condition::Kind::Not:
            for (const Condition& child : c.children) bind_condition(base, child, having);
            return;
        default:
            break;
    }
    if (!c.aggregate.empty()) {
        if (!having) throw errors::malformed_query("aggregate functions are only allowed in HAVING");
        for (const Operand& o : c.values) {
            if (!o.value.is_numeric() && !o.value.is_null()) {
                throw errors::malformed_query("HAVING compares aggregates with numbers");
            }
        }
        if (!c.field.parts.empty()) resolve(base, c.field, false);
        return;
    }
    const Resolved& rp = resolve(base, c.field, false);
    if (c.kind == Condition::Kind::Like) {
        const FieldType t = rp.polymorphic ? FieldType::Text : rp.field->value_type();
        if (!is_text_like(t)) throw errors::malformed_query("LIKE can only be used with text fields");
        return;
    }
    if ((c.kind == Condition::Kind::Includes || c.kind == Condition::Kind::Excludes) &&
        (rp.polymorphic || rp.field->type != FieldType::MultiPicklist)) {
        throw errors::malformed_query("INCLUDES and EXCLUDES only work with multi-select picklists");
    }
    check_operand_types(rp, c);
}

bool Executor::compare(const FieldValue& lhs, const std::string& op, const Operand& rhs,
                       const meta::FieldDef* field) const {
    if (rhs.kind == Operand::Kind::DateRange) {
        if (lhs.is_null()) return op == "!=";
        const auto [start, end] = date_range(rhs.literal);
        const int64_t d = lhs.kind() == ValueKind::DateTime ? org_.tz().local_date(lhs.as_datetime()) : lhs.as_date();
        if (op == "=") return d >= start && d < end;
        if (op == "!=") return d < start || d >= end;
        if (op == "<") return d < start;
        if (op == "<=") return d < end;
        if (op == ">") return d >= end;
        if (op == ">=") return d >= start;
        return false;
    }
    const FieldValue& v = rhs.value;
    if (v.is_null()) {
        if (op == "=") return lhs.is_null();
        if (op == "!=") return !lhs.is_null();
        return false;
    }
    if (lhs.is_null()) return op == "!=";

    int c = 0;
    if (lhs.kind() == ValueKind::String && v.kind() == ValueKind::String) {
        if (field != nullptr && (meta::is_reference_type(field->type) || field->type == FieldType::Id)) {
            std::string b18;
            const std::string& a = lhs.as_string();
            const std::string& b = ids::normalize(v.as_string(), &b18) ? b18 : v.as_string();
            c = a.compare(b);
        } else {
            c = str::icompare(lhs.as_string(), v.as_string());
        }
    } else if (lhs.is_numeric() && v.is_numeric()) {
        const double a = lhs.as_double(), b = v.as_double();
        c = a < b ? -1 : (a > b ? 1 : 0);
    } else if (lhs.kind() == ValueKind::DateTime && v.kind() == ValueKind::Date) {
        const int64_t b = v.as_date() * timeutil::kMsPerDay;
        c = lhs.as_datetime() < b ? -1 : (lhs.as_datetime() > b ? 1 : 0);
    } else {
        c = lhs.compare(v);
    }
    if (op == "=") return c == 0;
    if (op == "!=") return c != 0;
    if (op == "<") return c < 0;
    if (op == "<=") return c <= 0;
    if (op == ">") return c > 0;
    if (op == ">=") return c >= 0;
    return false;
}

bool Executor::eval(const meta::ObjectDef& base, const Condition& c, const Record& r) {
    switch (c.kind) {
        case Condition::Kind::And:
            for (const Condition& child : c.children) {
                if (!eval(base, child, r)) return false;
            }
            return true;
        case Condition::Kind::Or:
            for (const Condition& child : c.children) {
                if (eval(base, child, r)) return true;
            }
            return false;
        case Condition::Kind::Not: return !eval(base, c.children[0], r);
        default: break;
    }
    const Resolved& rp = resolve(base, c.field, false);
    const FieldValue lhs = value_of(r, rp);
    switch (c.kind) {
        case Condition::Kind::Compare: return compare(lhs, c.op, c.values[0], rp.field);
        case Condition::Kind::In: {
            bool found = false;
            for (const Operand& o : c.values) {
                if (compare(lhs, "=", o, rp.field)) {
                    found = true;
                    break;
                }
            }
            return c.negated ? !found : found;
        }
        case Condition::Kind::Like: {
            if (lhs.is_null()) return c.negated;
            const bool m = like_match(lhs.to_text(), c.values[0].value.as_string());
            return c.negated ? !m : m;
        }
        case Condition::Kind::Includes:
        case Condition::Kind::Excludes: {
            const std::vector<std::string> have = str::split(lhs.to_text(), ';');
            bool any = false;
            for (const Operand& o : c.values) {
                // Each operand is "a;b": all of a and b must be present.
                bool all = true;
                for (const std::string& want : str::split_trimmed(o.value.to_text(), ';')) {
                    if (std::find(have.begin(), have.end(), want) == have.end()) {
                        all = false;
                        break;
                    }
                }
                if (all) {
                    any = true;
                    break;
                }
            }
            return c.kind == Condition::Kind::Includes ? any : !any;
        }
        default: return false;
    }
}

std::vector<const Record*> Executor::candidates(const meta::ObjectDef& obj, const Query& q) {
    std::vector<const Record*> out;
    if (obj.name == "User") {
        if (user_rows_.empty()) {
            for (const auth::User& u : org_.users.all()) user_rows_.push_back(engine::user_record(u, org_));
        }
        for (const Record& r : user_rows_) out.push_back(&r);
        return out;
    }
    const storage::Table* table = org_.db.find_table(obj.name);
    if (table == nullptr) return out;

    // Use an index for "Field = value" at the top level (or inside a top-level AND).
    const Condition* eq = nullptr;
    if (q.where) {
        const Condition& w = *q.where;
        auto usable = [&](const Condition& c) {
            return c.kind == Condition::Kind::Compare && c.op == "=" && c.field.simple() && c.aggregate.empty() &&
                   c.values[0].kind == Operand::Kind::Value && !c.values[0].value.is_null() &&
                   table->index(obj.field(c.field.last()) ? obj.field(c.field.last())->name : "") != nullptr;
        };
        if (usable(w)) {
            eq = &w;
        } else if (w.kind == Condition::Kind::And) {
            for (const Condition& child : w.children) {
                if (usable(child)) {
                    eq = &child;
                    break;
                }
            }
        }
    }
    if (eq != nullptr) {
        const meta::FieldDef* f = obj.field(eq->field.last());
        FieldValue key = eq->values[0].value;
        if (meta::is_reference_type(f->type) && key.kind() == ValueKind::String) {
            std::string id18;
            if (ids::normalize(key.as_string(), &id18)) key = FieldValue::string(id18);
        } else if (meta::is_numeric_type(f->value_type()) && key.is_numeric()) {
            key = FieldValue::number(key.as_double());
        }
        for (const std::string& id : table->index(f->name)->find(key)) {
            if (const Record* r = table->get(id)) out.push_back(r);
        }
        return out;
    }
    out.reserve(table->size());
    for (const auto& [id, r] : table->rows()) out.push_back(&r);
    return out;
}

std::vector<const Record*> Executor::filter(const meta::ObjectDef& obj, const Query& q, bool include_deleted) {
    std::vector<const Record*> rows;
    for (const Record* r : candidates(obj, q)) {
        if (!include_deleted && !q.all_rows && r->is_deleted()) continue;
        if (q.where && !eval(obj, *q.where, *r)) continue;
        if (!access_.can_read(obj, *r)) continue;
        rows.push_back(r);
    }
    return rows;
}

void Executor::sort(const meta::ObjectDef& obj, const std::vector<OrderItem>& order, std::vector<const Record*>* rows) {
    if (order.empty()) {
        std::sort(rows->begin(), rows->end(), [](const Record* a, const Record* b) { return a->id < b->id; });
        return;
    }
    std::vector<const Resolved*> keys;
    for (const OrderItem& o : order) {
        if (!o.aggregate.empty()) throw errors::malformed_query("ORDER BY an aggregate needs GROUP BY");
        keys.push_back(&resolve(obj, o.path, false));
    }
    // Precompute sort keys so relationship lookups happen once per row.
    std::vector<std::pair<std::vector<FieldValue>, const Record*>> decorated;
    decorated.reserve(rows->size());
    for (const Record* r : *rows) {
        std::vector<FieldValue> k;
        for (const Resolved* rp : keys) k.push_back(value_of(*r, *rp));
        decorated.emplace_back(std::move(k), r);
    }
    std::stable_sort(decorated.begin(), decorated.end(), [&](const auto& a, const auto& b) {
        for (size_t i = 0; i < order.size(); ++i) {
            const FieldValue& x = a.first[i];
            const FieldValue& y = b.first[i];
            if (x.is_null() != y.is_null()) {
                bool nulls_first = !order[i].descending;
                if (order[i].nulls == OrderItem::Nulls::First) nulls_first = true;
                if (order[i].nulls == OrderItem::Nulls::Last) nulls_first = false;
                return x.is_null() ? nulls_first : !nulls_first;
            }
            const int c = compare_for_sort(x, y);
            if (c != 0) return order[i].descending ? c > 0 : c < 0;
        }
        return a.second->id < b.second->id;
    });
    for (size_t i = 0; i < decorated.size(); ++i) (*rows)[i] = decorated[i].second;
}

json::Value Executor::child_rows(const meta::ObjectDef& parent, const Record& r, const Query& sub) {
    const meta::ChildRelationship* rel = nullptr;
    for (const meta::ChildRelationship& c : parent.children) {
        if (str::iequals(c.relationship_name, sub.from)) {
            rel = &c;
            break;
        }
    }
    if (rel == nullptr) {
        throw errors::bad_request("INVALID_TYPE", "Didn't understand relationship '" + sub.from + "' in FROM part of "
                                                  "query call");
    }
    const meta::ObjectDef* child = org_.schema.object(rel->child_object);
    access_.require_object(*child, engine::ObjectOp::Read);
    const storage::Table* table = org_.db.find_table(child->name);
    if (table == nullptr) return json::Value();
    std::vector<const Record*> rows;
    const storage::Index* idx = table->index(rel->field);
    std::vector<std::string> ids_found;
    if (idx != nullptr) {
        ids_found = idx->find(FieldValue::string(r.id));
    } else {
        for (const auto& [id, row] : table->rows()) {
            if (row.text(rel->field) == r.id) ids_found.push_back(id);
        }
    }
    for (const std::string& id : ids_found) {
        const Record* c = table->get(id);
        if (c == nullptr || c->is_deleted()) continue;
        if (sub.where && !eval(*child, *sub.where, *c)) continue;
        if (!access_.can_read(*child, *c)) continue;
        rows.push_back(c);
    }
    if (rows.empty()) return json::Value();
    sort(*child, sub.order_by, &rows);
    if (sub.limit >= 0 && static_cast<int64_t>(rows.size()) > sub.limit) rows.resize(static_cast<size_t>(sub.limit));
    json::Value out = json::Value::object();
    out.set("totalSize", rows.size());
    out.set("done", true);
    json::Value recs = json::Value::array();
    for (const Record* c : rows) recs.push_back(build_row(*child, *c, sub));
    out.set("records", std::move(recs));
    return out;
}

json::Value Executor::build_row(const meta::ObjectDef& obj, const Record& r, const Query& q) {
    json::Value row = json::Value::object();
    row.set("attributes", engine::record_attributes(obj.name, r.id));
    for (const SelectItem& item : q.select) {
        if (item.kind == SelectItem::Kind::Subquery) {
            row.set(item.subquery->from, child_rows(obj, r, *item.subquery));
            continue;
        }
        const Resolved& rp = resolve(obj, item.path, true);
        if (rp.lookups.empty()) {
            row.set(rp.field->name, meta::value_to_json(*rp.field, r.get(rp.field->name)));
            continue;
        }
        // Nest relationship values: {"Account": {"attributes": ..., "Name": ...}}
        json::Value* node = &row;
        const Record* cur = &r;
        const meta::ObjectDef* cur_obj = &obj;
        std::vector<Record> chain;
        chain.reserve(rp.lookups.size());
        bool missing = false;
        for (size_t i = 0; i < rp.lookups.size(); ++i) {
            const meta::FieldDef* link = rp.lookups[i];
            const std::string rel_name = link->relationship_name;
            const FieldValue& id = cur->get(link->name);
            if (id.kind() != ValueKind::String) {
                if (!node->contains(rel_name)) node->set(rel_name, json::Value());
                missing = true;
                break;
            }
            const meta::ObjectDef* parent_obj = nullptr;
            std::optional<Record> parent = engine::load_record(org_, id.as_string(), &parent_obj);
            if (!parent) {
                if (!node->contains(rel_name)) node->set(rel_name, json::Value());
                missing = true;
                break;
            }
            chain.push_back(std::move(*parent));
            cur = &chain.back();
            cur_obj = parent_obj;
            json::Value* child = node->find(rel_name);
            if (child == nullptr || !child->is_object()) {
                json::Value fresh = json::Value::object();
                fresh.set("attributes", engine::record_attributes(cur_obj->name, cur->id));
                node->set(rel_name, std::move(fresh));
                child = node->find(rel_name);
            }
            node = child;
        }
        if (missing) continue;
        if (rp.polymorphic) {
            node->set(rp.polymorphic_field, rp.polymorphic_field == "Id"
                                                ? json::Value(cur->id)
                                                : json::Value(engine::record_name(*cur_obj, *cur)));
        } else {
            node->set(rp.field->name, meta::value_to_json(*rp.field, cur->get(rp.field->name)));
        }
    }
    return row;
}

QueryResult Executor::run_aggregate(const meta::ObjectDef& obj, const Query& q, bool include_deleted) {
    struct AggSpec {
        std::string key;  // FUNC(path)
        std::string function;
        const Resolved* field = nullptr;  // null for COUNT()
    };
    std::vector<AggSpec> specs;
    auto add_spec = [&](const std::string& function, const FieldPath& path, bool star) {
        AggSpec s;
        s.function = function;
        s.key = function + "(" + (star ? std::string() : str::to_lower(path.text())) + ")";
        for (const AggSpec& existing : specs) {
            if (existing.key == s.key) return;
        }
        if (!star) {
            s.field = &resolve(obj, path, true);
            const FieldType t = s.field->polymorphic ? FieldType::Text : s.field->field->value_type();
            if ((function == "SUM" || function == "AVG") && !meta::is_numeric_type(t)) {
                throw errors::malformed_query("field " + path.text() + " does not support aggregate operator " + function);
            }
        }
        specs.push_back(std::move(s));
    };
    for (const SelectItem& s : q.select) {
        if (s.kind == SelectItem::Kind::Subquery) throw errors::malformed_query("subqueries can't be used with GROUP BY");
        if (s.kind == SelectItem::Kind::Aggregate) {
            add_spec(s.function, s.path, s.count_star);
        } else {
            bool grouped = false;
            for (const FieldPath& g : q.group_by) {
                if (str::iequals(g.text(), s.path.text())) grouped = true;
            }
            if (!grouped) {
                throw errors::malformed_query("Field must be grouped or aggregated: " + s.path.text());
            }
        }
    }
    std::function<void(const Condition&)> collect_having = [&](const Condition& c) {
        for (const Condition& child : c.children) collect_having(child);
        if (!c.aggregate.empty()) add_spec(c.aggregate, c.field, c.field.parts.empty());
    };
    if (q.having) collect_having(*q.having);
    for (const OrderItem& o : q.order_by) {
        if (!o.aggregate.empty()) add_spec(o.aggregate, o.path, o.path.parts.empty());
    }

    std::vector<const Resolved*> group_fields;
    for (const FieldPath& g : q.group_by) group_fields.push_back(&resolve(obj, g, true));

    struct GroupState {
        std::vector<int64_t> count;
        std::vector<double> sum;
        std::vector<FieldValue> min;
        std::vector<FieldValue> max;
        std::vector<std::set<FieldValue>> distinct;
    };
    std::map<std::vector<FieldValue>, GroupState> groups;
    for (const Record* r : filter(obj, q, include_deleted)) {
        std::vector<FieldValue> key;
        for (const Resolved* g : group_fields) key.push_back(value_of(*r, *g));
        GroupState& st = groups[key];
        if (st.count.empty()) {
            st.count.assign(specs.size(), 0);
            st.sum.assign(specs.size(), 0.0);
            st.min.assign(specs.size(), FieldValue());
            st.max.assign(specs.size(), FieldValue());
            st.distinct.resize(specs.size());
        }
        for (size_t i = 0; i < specs.size(); ++i) {
            const AggSpec& s = specs[i];
            if (s.field == nullptr) {
                ++st.count[i];
                continue;
            }
            const FieldValue v = value_of(*r, *s.field);
            if (v.is_null()) continue;
            ++st.count[i];
            if (v.is_numeric()) st.sum[i] += v.as_double();
            if (st.min[i].is_null() || compare_for_sort(v, st.min[i]) < 0) st.min[i] = v;
            if (st.max[i].is_null() || compare_for_sort(v, st.max[i]) > 0) st.max[i] = v;
            if (s.function == "COUNT_DISTINCT") st.distinct[i].insert(v);
        }
    }
    // A query with aggregates but no GROUP BY always returns one row.
    if (groups.empty() && q.group_by.empty()) {
        GroupState& st = groups[{}];
        st.count.assign(specs.size(), 0);
        st.sum.assign(specs.size(), 0.0);
        st.min.assign(specs.size(), FieldValue());
        st.max.assign(specs.size(), FieldValue());
        st.distinct.resize(specs.size());
    }

    auto agg_value = [&](const GroupState& st, size_t i) -> FieldValue {
        const AggSpec& s = specs[i];
        if (s.function == "COUNT") return FieldValue::integer(st.count[i]);
        if (s.function == "COUNT_DISTINCT") return FieldValue::integer(static_cast<int64_t>(st.distinct[i].size()));
        if (s.function == "SUM") return st.count[i] == 0 ? FieldValue::null() : FieldValue::number(st.sum[i]);
        if (s.function == "AVG") return FieldValue::number(st.sum[i] / static_cast<double>(st.count[i]));
        if (s.function == "MIN") return st.min[i];
        return st.max[i];
    };
    auto spec_index = [&](const std::string& key) -> size_t {
        for (size_t i = 0; i < specs.size(); ++i) {
            if (specs[i].key == key) return i;
        }
        throw errors::malformed_query("unknown aggregate " + key);
    };

    struct OutRow {
        std::vector<FieldValue> key;
        std::vector<FieldValue> aggs;
    };
    std::vector<OutRow> out_rows;
    for (const auto& [key, st] : groups) {
        OutRow row;
        row.key = key;
        for (size_t i = 0; i < specs.size(); ++i) row.aggs.push_back(agg_value(st, i));
        if (q.having) {
            std::function<bool(const Condition&)> having = [&](const Condition& c) -> bool {
                switch (c.kind) {
                    case Condition::Kind::And:
                        for (const Condition& ch : c.children) {
                            if (!having(ch)) return false;
                        }
                        return true;
                    case Condition::Kind::Or:
                        for (const Condition& ch : c.children) {
                            if (having(ch)) return true;
                        }
                        return false;
                    case Condition::Kind::Not: return !having(c.children[0]);
                    default: break;
                }
                FieldValue lhs;
                const meta::FieldDef* field = nullptr;
                if (!c.aggregate.empty()) {
                    lhs = row.aggs[spec_index(condition_key(c))];
                } else {
                    size_t gi = q.group_by.size();
                    for (size_t g = 0; g < q.group_by.size(); ++g) {
                        if (str::iequals(q.group_by[g].text(), c.field.text())) gi = g;
                    }
                    if (gi == q.group_by.size()) throw errors::malformed_query("HAVING field must be grouped");
                    lhs = row.key[gi];
                    field = group_fields[gi]->field;
                }
                if (c.kind == Condition::Kind::In) {
                    bool found = false;
                    for (const Operand& o : c.values) found = found || compare(lhs, "=", o, field);
                    return c.negated ? !found : found;
                }
                return compare(lhs, c.op, c.values[0], field);
            };
            if (!having(*q.having)) continue;
        }
        out_rows.push_back(std::move(row));
    }

    if (!q.order_by.empty()) {
        std::vector<std::pair<bool, size_t>> order_src;  // (is_aggregate, index)
        for (const OrderItem& o : q.order_by) {
            if (!o.aggregate.empty()) {
                order_src.emplace_back(true, spec_index(order_key(o)));
                continue;
            }
            size_t gi = q.group_by.size();
            for (size_t g = 0; g < q.group_by.size(); ++g) {
                if (str::iequals(q.group_by[g].text(), o.path.text())) gi = g;
            }
            if (gi == q.group_by.size()) throw errors::malformed_query("ORDER BY field must be grouped: " + o.path.text());
            order_src.emplace_back(false, gi);
        }
        std::stable_sort(out_rows.begin(), out_rows.end(), [&](const OutRow& a, const OutRow& b) {
            for (size_t i = 0; i < order_src.size(); ++i) {
                const FieldValue& x = order_src[i].first ? a.aggs[order_src[i].second] : a.key[order_src[i].second];
                const FieldValue& y = order_src[i].first ? b.aggs[order_src[i].second] : b.key[order_src[i].second];
                const int c = compare_for_sort(x, y);
                if (c != 0) return q.order_by[i].descending ? c > 0 : c < 0;
            }
            return false;
        });
    }
    if (q.offset > 0) {
        const size_t off = std::min(out_rows.size(), static_cast<size_t>(q.offset));
        out_rows.erase(out_rows.begin(), out_rows.begin() + static_cast<std::ptrdiff_t>(off));
    }
    if (q.limit >= 0 && static_cast<int64_t>(out_rows.size()) > q.limit) out_rows.resize(static_cast<size_t>(q.limit));

    QueryResult result;
    if (q.is_count_only()) {
        result.total_size = out_rows.empty() ? 0 : out_rows[0].aggs[0].as_int();
        return result;
    }
    int expr = 0;
    for (const OutRow& row : out_rows) {
        json::Value rec = json::Value::object();
        json::Value attrs = json::Value::object();
        attrs.set("type", "AggregateResult");
        rec.set("attributes", std::move(attrs));
        expr = 0;
        for (const SelectItem& s : q.select) {
            if (s.kind == SelectItem::Kind::Aggregate) {
                const size_t i = spec_index(s.expression_key());
                const std::string name = s.alias.empty() ? "expr" + std::to_string(expr++) : s.alias;
                const FieldValue& v = row.aggs[i];
                const meta::FieldDef* f = specs[i].field && !specs[i].field->polymorphic ? specs[i].field->field : nullptr;
                if (f != nullptr && (s.function == "MIN" || s.function == "MAX")) {
                    rec.set(name, meta::value_to_json(*f, v));
                } else if (v.is_null()) {
                    rec.set(name, json::Value());
                } else if (v.kind() == ValueKind::Int) {
                    rec.set(name, v.as_int());
                } else {
                    rec.set(name, v.as_double());
                }
            } else {
                for (size_t g = 0; g < q.group_by.size(); ++g) {
                    if (!str::iequals(q.group_by[g].text(), s.path.text())) continue;
                    const Resolved* rp = group_fields[g];
                    const std::string name = rp->polymorphic ? rp->polymorphic_field : rp->field->name;
                    rec.set(name, rp->polymorphic ? json::Value(row.key[g].to_text())
                                                  : meta::value_to_json(*rp->field, row.key[g]));
                }
            }
        }
        result.records.push_back(std::move(rec));
    }
    result.total_size = static_cast<int64_t>(out_rows.size());
    return result;
}

std::vector<std::string> Executor::matching_ids(const Query& q, bool include_deleted) {
    const meta::ObjectDef* obj = org_.schema.object(q.from);
    if (obj == nullptr) throw errors::bad_request("INVALID_TYPE", "sObject type '" + q.from + "' is not supported.");
    access_.require_object(*obj, engine::ObjectOp::Read);
    if (q.where) bind_condition(*obj, *q.where, false);
    std::vector<const Record*> rows = filter(*obj, q, include_deleted);
    sort(*obj, q.order_by, &rows);
    std::vector<std::string> out;
    const size_t start = std::min(rows.size(), static_cast<size_t>(std::max<int64_t>(0, q.offset)));
    for (size_t i = start; i < rows.size(); ++i) {
        if (q.limit >= 0 && static_cast<int64_t>(out.size()) >= q.limit) break;
        out.push_back(rows[i]->id);
    }
    return out;
}

QueryResult Executor::run(const Query& q, const ExecOptions& options, CursorStore* cursors) {
    const meta::ObjectDef* obj = org_.schema.object(q.from);
    if (obj == nullptr || !obj->queryable) {
        throw errors::bad_request("INVALID_TYPE", "sObject type '" + q.from + "' is not supported.");
    }
    access_.require_object(*obj, engine::ObjectOp::Read);
    if (q.offset > kMaxOffset) throw errors::malformed_query("maximum OFFSET is " + std::to_string(kMaxOffset));
    if (q.where) bind_condition(*obj, *q.where, false);
    if (q.having) bind_condition(*obj, *q.having, true);
    const bool include_deleted = options.include_deleted || q.all_rows;

    if (q.is_aggregate()) return run_aggregate(*obj, q, include_deleted);

    // Validate the select list before touching data.
    for (const SelectItem& s : q.select) {
        if (s.kind == SelectItem::Kind::Field) resolve(*obj, s.path, true);
    }

    std::vector<const Record*> rows = filter(*obj, q, include_deleted);
    sort(*obj, q.order_by, &rows);
    if (q.offset > 0) rows.erase(rows.begin(), rows.begin() + q.offset);
    if (q.limit >= 0 && static_cast<int64_t>(rows.size()) > q.limit) rows.resize(static_cast<size_t>(q.limit));

    QueryResult result;
    result.total_size = static_cast<int64_t>(rows.size());
    const int64_t batch = std::max<int64_t>(1, options.batch_size);
    std::deque<json::Value> rest;
    for (size_t i = 0; i < rows.size(); ++i) {
        json::Value row = build_row(*obj, *rows[i], q);
        if (static_cast<int64_t>(i) < batch) {
            result.records.push_back(std::move(row));
        } else {
            rest.push_back(std::move(row));
        }
    }
    if (!rest.empty() && cursors != nullptr) {
        result.done = false;
        result.next_cursor = cursors->open(org_.id(), who_.user_id, std::move(rest), result.total_size, batch);
    }
    return result;
}

QueryResult execute(const std::string& tql, const engine::Principal& who, const ExecOptions& options,
                    CursorStore* cursors) {
    const Query q = parse(tql);
    std::shared_lock<std::shared_mutex> lock(who.org->mutex());
    Executor ex(*who.org, who);
    return ex.run(q, options, cursors);
}

}  // namespace tcrm::query
