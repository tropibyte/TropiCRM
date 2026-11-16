// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <array>
#include <cmath>
#include <regex>

#include "formula/internal.hpp"
#include "util/strutil.hpp"

namespace tcrm::formula {
namespace {

constexpr std::array<FunctionInfo, 52> kFunctions = {{
    {"IF", 3, 3},          {"CASE", 3, -1},        {"AND", 1, -1},        {"OR", 1, -1},
    {"NOT", 1, 1},         {"ISBLANK", 1, 1},      {"ISNULL", 1, 1},      {"BLANKVALUE", 2, 2},
    {"NULLVALUE", 2, 2},   {"LEN", 1, 1},          {"LEFT", 2, 2},        {"RIGHT", 2, 2},
    {"MID", 3, 3},         {"UPPER", 1, 1},        {"LOWER", 1, 1},       {"TRIM", 1, 1},
    {"CONTAINS", 2, 2},    {"BEGINS", 2, 2},       {"FIND", 2, 3},        {"SUBSTITUTE", 3, 3},
    {"TEXT", 1, 1},        {"VALUE", 1, 1},        {"LPAD", 2, 3},        {"RPAD", 2, 3},
    {"REGEX", 2, 2},       {"ROUND", 2, 2},        {"FLOOR", 1, 1},       {"CEILING", 1, 1},
    {"ABS", 1, 1},         {"MAX", 1, -1},         {"MIN", 1, -1},        {"MOD", 2, 2},
    {"SQRT", 1, 1},        {"TODAY", 0, 0},        {"NOW", 0, 0},         {"DATE", 3, 3},
    {"DATEVALUE", 1, 1},   {"DATETIMEVALUE", 1, 1}, {"YEAR", 1, 1},       {"MONTH", 1, 1},
    {"DAY", 1, 1},         {"WEEKDAY", 1, 1},      {"ADDMONTHS", 2, 2},   {"ISPICKVAL", 2, 2},
    {"INCLUDES", 2, 2},    {"ISCHANGED", 1, 1},    {"ISNEW", 0, 0},       {"PRIORVALUE", 1, 1},
    {"ISNUMBER", 1, 1},    {"HYPERLINK", 2, 2},    {"BR", 0, 0},          {"IMAGE", 2, 3},
}};

[[noreturn]] void fail(const std::string& msg, size_t pos) {
    throw FormulaError(msg, pos);
}

FieldValue num(double d) {
    if (d == std::floor(d) && std::fabs(d) < 9e15) return FieldValue::integer(static_cast<int64_t>(d));
    return FieldValue::number(d);
}

int compare_values(const FieldValue& a, const FieldValue& b, size_t pos) {
    if (a.is_numeric() && b.is_numeric()) {
        const double x = a.as_double(), y = b.as_double();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.kind() == ValueKind::String || b.kind() == ValueKind::String) {
        const std::string x = to_text(a), y = to_text(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.kind() == b.kind()) return a.compare(b);
    if (a.is_temporal() && b.is_temporal()) {
        const int64_t x = a.kind() == ValueKind::Date ? a.as_date() * timeutil::kMsPerDay : a.as_datetime();
        const int64_t y = b.kind() == ValueKind::Date ? b.as_date() * timeutil::kMsPerDay : b.as_datetime();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    fail(std::string("can't compare ") + value_kind_name(a.kind()) + " with " + value_kind_name(b.kind()), pos);
}

FieldValue arithmetic(const std::string& op, const FieldValue& a, const FieldValue& b, size_t pos) {
    if (a.is_null() || b.is_null()) return FieldValue::null();
    // Date arithmetic: date +/- days, date - date.
    if (a.kind() == ValueKind::Date && b.is_numeric() && (op == "+" || op == "-")) {
        const int64_t days = static_cast<int64_t>(b.as_double());
        return FieldValue::date(op == "+" ? a.as_date() + days : a.as_date() - days);
    }
    if (a.kind() == ValueKind::DateTime && b.is_numeric() && (op == "+" || op == "-")) {
        const int64_t ms = static_cast<int64_t>(b.as_double() * static_cast<double>(timeutil::kMsPerDay));
        return FieldValue::datetime(op == "+" ? a.as_datetime() + ms : a.as_datetime() - ms);
    }
    if (op == "-" && a.kind() == ValueKind::Date && b.kind() == ValueKind::Date) {
        return FieldValue::integer(a.as_date() - b.as_date());
    }
    if (op == "-" && a.kind() == ValueKind::DateTime && b.kind() == ValueKind::DateTime) {
        return FieldValue::number(static_cast<double>(a.as_datetime() - b.as_datetime()) /
                                  static_cast<double>(timeutil::kMsPerDay));
    }
    if (op == "+" && (a.kind() == ValueKind::String || b.kind() == ValueKind::String)) {
        return FieldValue::string(to_text(a) + to_text(b));
    }
    const double x = to_number(a, pos);
    const double y = to_number(b, pos);
    if (op == "+") return num(x + y);
    if (op == "-") return num(x - y);
    if (op == "*") return num(x * y);
    if (op == "/") {
        if (y == 0) fail("division by zero", pos);
        return num(x / y);
    }
    if (op == "^") return num(std::pow(x, y));
    fail("unknown operator " + op, pos);
}

int64_t to_int(const FieldValue& v, size_t pos) {
    return static_cast<int64_t>(std::floor(to_number(v, pos)));
}

int64_t to_date(const FieldValue& v, Context& ctx, size_t pos) {
    if (v.kind() == ValueKind::Date) return v.as_date();
    if (v.kind() == ValueKind::DateTime) return ctx.timezone().local_date(v.as_datetime());
    if (v.kind() == ValueKind::String) {
        int64_t d = 0;
        if (timeutil::parse_date(v.as_string(), &d)) return d;
    }
    fail("expected a date", pos);
}

std::regex compile_regex(const std::string& pattern, size_t pos) {
    try {
        return std::regex(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        fail("invalid regular expression", pos);
    }
}

FieldValue call(const Node& n, Context& ctx) {
    const std::string& f = n.name;
    const size_t pos = n.pos;

    // Functions that must not evaluate all their arguments eagerly.
    if (f == "IF") {
        return truthy(eval_node(*n.args[0], ctx)) ? eval_node(*n.args[1], ctx) : eval_node(*n.args[2], ctx);
    }
    if (f == "AND") {
        for (const NodePtr& a : n.args) {
            if (!truthy(eval_node(*a, ctx))) return FieldValue::boolean(false);
        }
        return FieldValue::boolean(true);
    }
    if (f == "OR") {
        for (const NodePtr& a : n.args) {
            if (truthy(eval_node(*a, ctx))) return FieldValue::boolean(true);
        }
        return FieldValue::boolean(false);
    }
    if (f == "CASE") {
        const FieldValue subject = eval_node(*n.args[0], ctx);
        size_t i = 1;
        for (; i + 1 < n.args.size(); i += 2) {
            const FieldValue candidate = eval_node(*n.args[i], ctx);
            if (!subject.is_null() && !candidate.is_null() && compare_values(subject, candidate, pos) == 0) {
                return eval_node(*n.args[i + 1], ctx);
            }
        }
        if (i < n.args.size()) return eval_node(*n.args[i], ctx);
        fail("CASE needs an else value", pos);
    }
    if (f == "BLANKVALUE" || f == "NULLVALUE") {
        FieldValue v = eval_node(*n.args[0], ctx);
        return is_blank(v) ? eval_node(*n.args[1], ctx) : v;
    }
    if (f == "ISCHANGED" || f == "PRIORVALUE") {
        if (n.args[0]->kind != Node::Kind::Field) fail(f + " needs a field", pos);
        const std::string& field = n.args[0]->name;
        if (f == "PRIORVALUE") return ctx.is_new() ? FieldValue::null() : ctx.prior(field);
        if (ctx.is_new()) return FieldValue::boolean(false);
        return FieldValue::boolean(ctx.prior(field) != ctx.field(field));
    }
    if (f == "ISNEW") return FieldValue::boolean(ctx.is_new());
    if (f == "TODAY") return FieldValue::date(ctx.timezone().local_date(ctx.now_ms()));
    if (f == "NOW") return FieldValue::datetime(ctx.now_ms());
    if (f == "BR") return FieldValue::string("\n");

    std::vector<FieldValue> a;
    a.reserve(n.args.size());
    for (const NodePtr& arg : n.args) a.push_back(eval_node(*arg, ctx));

    if (f == "NOT") return FieldValue::boolean(!truthy(a[0]));
    if (f == "ISBLANK" || f == "ISNULL") return FieldValue::boolean(is_blank(a[0]));
    if (f == "ISNUMBER") {
        double d = 0;
        return FieldValue::boolean(a[0].is_numeric() || (a[0].kind() == ValueKind::String &&
                                                         str::parse_double(str::trim(a[0].as_string()), &d)));
    }
    if (f == "LEN") return FieldValue::integer(static_cast<int64_t>(to_text(a[0]).size()));
    if (f == "UPPER") return a[0].is_null() ? a[0] : FieldValue::string(str::to_upper(to_text(a[0])));
    if (f == "LOWER") return a[0].is_null() ? a[0] : FieldValue::string(str::to_lower(to_text(a[0])));
    if (f == "TRIM") return a[0].is_null() ? a[0] : FieldValue::string(std::string(str::trim(to_text(a[0]))));
    if (f == "LEFT") {
        const std::string s = to_text(a[0]);
        const int64_t k = std::max<int64_t>(0, to_int(a[1], pos));
        return FieldValue::string(s.substr(0, static_cast<size_t>(k)));
    }
    if (f == "RIGHT") {
        const std::string s = to_text(a[0]);
        const int64_t k = std::clamp<int64_t>(to_int(a[1], pos), 0, static_cast<int64_t>(s.size()));
        return FieldValue::string(s.substr(s.size() - static_cast<size_t>(k)));
    }
    if (f == "MID") {
        const std::string s = to_text(a[0]);
        const int64_t start = std::max<int64_t>(1, to_int(a[1], pos));
        const int64_t len = std::max<int64_t>(0, to_int(a[2], pos));
        if (static_cast<size_t>(start) > s.size()) return FieldValue::string("");
        return FieldValue::string(s.substr(static_cast<size_t>(start - 1), static_cast<size_t>(len)));
    }
    if (f == "CONTAINS") return FieldValue::boolean(to_text(a[0]).find(to_text(a[1])) != std::string::npos);
    if (f == "BEGINS") {
        const std::string s = to_text(a[0]), p = to_text(a[1]);
        return FieldValue::boolean(s.compare(0, p.size(), p) == 0);
    }
    if (f == "FIND") {
        const std::string needle = to_text(a[0]), hay = to_text(a[1]);
        size_t start = 0;
        if (a.size() == 3) start = static_cast<size_t>(std::max<int64_t>(1, to_int(a[2], pos)) - 1);
        const size_t at = needle.empty() ? std::string::npos : hay.find(needle, start);
        return FieldValue::integer(at == std::string::npos ? 0 : static_cast<int64_t>(at) + 1);
    }
    if (f == "SUBSTITUTE") return FieldValue::string(str::replace_all(to_text(a[0]), to_text(a[1]), to_text(a[2])));
    if (f == "TEXT") return a[0].is_null() ? FieldValue::string("") : FieldValue::string(to_text(a[0]));
    if (f == "VALUE") {
        double d = 0;
        if (!str::parse_double(str::trim(to_text(a[0])), &d)) fail("VALUE: not a number", pos);
        return num(d);
    }
    if (f == "LPAD" || f == "RPAD") {
        std::string s = to_text(a[0]);
        const int64_t width = std::clamp<int64_t>(to_int(a[1], pos), 0, 32000);
        const std::string pad = a.size() == 3 ? to_text(a[2]) : " ";
        if (static_cast<int64_t>(s.size()) >= width || pad.empty()) return FieldValue::string(s.substr(0, static_cast<size_t>(width)));
        std::string fill;
        while (static_cast<int64_t>(fill.size() + s.size()) < width) fill += pad;
        fill.resize(static_cast<size_t>(width) - s.size());
        return FieldValue::string(f == "LPAD" ? fill + s : s + fill);
    }
    if (f == "REGEX") {
        const std::regex re = compile_regex(to_text(a[1]), pos);
        const std::string s = to_text(a[0]);
        return FieldValue::boolean(std::regex_match(s, re));
    }
    if (f == "ROUND") {
        if (a[0].is_null()) return a[0];
        const double factor = std::pow(10.0, static_cast<double>(to_int(a[1], pos)));
        return num(std::round(to_number(a[0], pos) * factor) / factor);
    }
    if (f == "FLOOR") return a[0].is_null() ? a[0] : num(std::floor(to_number(a[0], pos)));
    if (f == "CEILING") return a[0].is_null() ? a[0] : num(std::ceil(to_number(a[0], pos)));
    if (f == "ABS") return a[0].is_null() ? a[0] : num(std::fabs(to_number(a[0], pos)));
    if (f == "SQRT") {
        const double d = to_number(a[0], pos);
        if (d < 0) fail("SQRT of a negative number", pos);
        return num(std::sqrt(d));
    }
    if (f == "MAX" || f == "MIN") {
        bool any = false;
        double best = 0;
        for (const FieldValue& v : a) {
            if (v.is_null()) continue;
            const double d = to_number(v, pos);
            if (!any || (f == "MAX" ? d > best : d < best)) best = d;
            any = true;
        }
        return any ? num(best) : FieldValue::null();
    }
    if (f == "MOD") {
        if (a[0].is_null() || a[1].is_null()) return FieldValue::null();
        const double x = to_number(a[0], pos), y = to_number(a[1], pos);
        if (x == std::floor(x) && y == std::floor(y)) {
            return FieldValue::integer(static_cast<int64_t>(x) % static_cast<int64_t>(y));
        }
        if (y == 0) fail("MOD by zero", pos);
        return num(std::fmod(x, y));
    }
    if (f == "DATE") {
        const int64_t y = to_int(a[0], pos), m = to_int(a[1], pos), d = to_int(a[2], pos);
        if (m < 1 || m > 12 || d < 1 || d > 31) fail("DATE: invalid date", pos);
        return FieldValue::date(timeutil::days_from_civil(static_cast<int>(y), static_cast<int>(m), static_cast<int>(d)));
    }
    if (f == "DATEVALUE") {
        if (a[0].is_null()) return a[0];
        return FieldValue::date(to_date(a[0], ctx, pos));
    }
    if (f == "DATETIMEVALUE") {
        if (a[0].kind() == ValueKind::DateTime) return a[0];
        if (a[0].kind() == ValueKind::Date) return FieldValue::datetime(a[0].as_date() * timeutil::kMsPerDay);
        int64_t ms = 0;
        if (!timeutil::parse_datetime(to_text(a[0]), &ms)) fail("DATETIMEVALUE: not a date-time", pos);
        return FieldValue::datetime(ms);
    }
    if (f == "YEAR" || f == "MONTH" || f == "DAY") {
        if (a[0].is_null()) return a[0];
        const timeutil::Civil c = timeutil::civil_from_days(to_date(a[0], ctx, pos));
        return FieldValue::integer(f == "YEAR" ? c.year : (f == "MONTH" ? c.month : c.day));
    }
    if (f == "WEEKDAY") {
        if (a[0].is_null()) return a[0];
        return FieldValue::integer(timeutil::day_of_week(to_date(a[0], ctx, pos)) + 1);
    }
    if (f == "ADDMONTHS") {
        if (a[0].is_null()) return a[0];
        return FieldValue::date(timeutil::add_months(to_date(a[0], ctx, pos), static_cast<int>(to_int(a[1], pos))));
    }
    if (f == "ISPICKVAL") return FieldValue::boolean(to_text(a[0]) == to_text(a[1]));
    if (f == "INCLUDES") {
        const std::string want = to_text(a[1]);
        for (const std::string& item : str::split(to_text(a[0]), ';')) {
            if (item == want) return FieldValue::boolean(true);
        }
        return FieldValue::boolean(false);
    }
    if (f == "HYPERLINK") return FieldValue::string(to_text(a[1]) + " (" + to_text(a[0]) + ")");
    if (f == "IMAGE") return FieldValue::string(to_text(a[1]));
    fail("unknown function " + f, pos);
}

}  // namespace

const FunctionInfo* find_function(const std::string& upper_name) {
    for (const FunctionInfo& fi : kFunctions) {
        if (upper_name == fi.name) return &fi;
    }
    return nullptr;
}

const std::vector<std::string>& function_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const FunctionInfo& fi : kFunctions) v.emplace_back(fi.name);
        return v;
    }();
    return names;
}

double to_number(const FieldValue& v, size_t pos) {
    switch (v.kind()) {
        case ValueKind::Null: return 0;
        case ValueKind::Int:
        case ValueKind::Double: return v.as_double();
        case ValueKind::Bool: return v.as_bool() ? 1 : 0;
        case ValueKind::String: {
            double d = 0;
            if (str::parse_double(str::trim(v.as_string()), &d)) return d;
            fail("expected a number, got text \"" + str::ellipsize(v.as_string(), 20) + "\"", pos);
        }
        default: fail(std::string("expected a number, got ") + value_kind_name(v.kind()), pos);
    }
}

std::string to_text(const FieldValue& v) {
    return v.to_text();
}

bool is_blank(const FieldValue& v) {
    return v.is_null() || (v.kind() == ValueKind::String && v.as_string().empty());
}

FieldValue eval_node(const Node& n, Context& ctx) {
    switch (n.kind) {
        case Node::Kind::Literal: return n.literal;
        case Node::Kind::Field:
            if (!n.name.empty() && n.name[0] == '$') return ctx.global(n.name);
            return ctx.field(n.name);
        case Node::Kind::Unary: {
            const FieldValue v = eval_node(*n.args[0], ctx);
            if (n.name == "!") return FieldValue::boolean(!truthy(v));
            if (v.is_null()) return v;
            return num(-to_number(v, n.pos));
        }
        case Node::Kind::Binary: {
            const std::string& op = n.name;
            if (op == "&&") {
                return FieldValue::boolean(truthy(eval_node(*n.args[0], ctx)) && truthy(eval_node(*n.args[1], ctx)));
            }
            if (op == "||") {
                return FieldValue::boolean(truthy(eval_node(*n.args[0], ctx)) || truthy(eval_node(*n.args[1], ctx)));
            }
            const FieldValue l = eval_node(*n.args[0], ctx);
            const FieldValue r = eval_node(*n.args[1], ctx);
            if (op == "&") return FieldValue::string(to_text(l) + to_text(r));
            if (op == "=" || op == "!=") {
                bool eq;
                if (is_blank(l) || is_blank(r)) {
                    eq = is_blank(l) && is_blank(r);
                } else {
                    eq = compare_values(l, r, n.pos) == 0;
                }
                return FieldValue::boolean(op == "=" ? eq : !eq);
            }
            if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                if (l.is_null() || r.is_null()) return FieldValue::boolean(false);
                const int c = compare_values(l, r, n.pos);
                if (op == "<") return FieldValue::boolean(c < 0);
                if (op == "<=") return FieldValue::boolean(c <= 0);
                if (op == ">") return FieldValue::boolean(c > 0);
                return FieldValue::boolean(c >= 0);
            }
            return arithmetic(op, l, r, n.pos);
        }
        case Node::Kind::Call: return call(n, ctx);
    }
    return FieldValue::null();
}

}  // namespace tcrm::formula
