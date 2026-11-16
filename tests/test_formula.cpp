// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <map>

#include "formula/formula.hpp"
#include "testing.hpp"
#include "util/time.hpp"

using namespace tcrm;
using formula::Formula;

namespace {

class MapContext : public formula::Context {
public:
    std::map<std::string, FieldValue> fields;
    std::map<std::string, FieldValue> prior_fields;
    bool fresh = false;
    int64_t now = 0;

    FieldValue field(const std::string& path) override {
        auto it = fields.find(path);
        if (it == fields.end()) throw formula::FormulaError("unknown field " + path, 0);
        return it->second;
    }
    FieldValue prior(const std::string& f) override {
        auto it = prior_fields.find(f);
        return it == prior_fields.end() ? field(f) : it->second;
    }
    bool is_new() override { return fresh; }
    FieldValue global(const std::string& name) override {
        if (name == "$User.Id") return FieldValue::string("005000000000001AAA");
        return FieldValue::null();
    }
    int64_t now_ms() override { return now; }
};

FieldValue eval(const std::string& src, MapContext& ctx) {
    return Formula::compile(src).evaluate(ctx);
}

FieldValue eval(const std::string& src) {
    MapContext ctx;
    REQUIRE(timeutil::parse_datetime("2026-06-15T14:30:00Z", &ctx.now));
    return eval(src, ctx);
}

}  // namespace

TEST(formula, arithmetic_and_precedence) {
    CHECK_EQ(eval("1 + 2 * 3").as_int(), int64_t{7});
    CHECK_EQ(eval("(1 + 2) * 3").as_int(), int64_t{9});
    CHECK_EQ(eval("2 ^ 3 ^ 2").as_int(), int64_t{512});
    CHECK_EQ(eval("-2 ^ 2").as_int(), int64_t{4});
    CHECK_NEAR(eval("7 / 2").as_double(), 3.5, 1e-12);
    CHECK_EQ(eval("10 - 4 - 3").as_int(), int64_t{3});
    CHECK_THROWS_AS(eval("1 / 0"), formula::FormulaError);
    CHECK_EQ(eval("\"a\" & 1 & true").as_string(), std::string("a1true"));
    CHECK_EQ(eval("\"x\" + \"y\"").as_string(), std::string("xy"));
}

TEST(formula, comparisons_and_logic) {
    CHECK(formula::truthy(eval("1 < 2 && 2 <= 2")));
    CHECK(formula::truthy(eval("3 > 2 || false")));
    CHECK(formula::truthy(eval("NOT(1 = 2)")));
    CHECK(formula::truthy(eval("1 <> 2")));
    CHECK(formula::truthy(eval("\"a\" = \"a\"")));
    CHECK(formula::truthy(eval("AND(true, 1 = 1, \"x\" != \"y\")")));
    CHECK(!formula::truthy(eval("OR(false, 1 = 2)")));
    CHECK(formula::truthy(eval("1 = 1 AND 2 = 2")));
    CHECK(formula::truthy(eval("!false")));
    // Blank compares equal only to blank.
    CHECK(formula::truthy(eval("null = \"\"")));
    CHECK(!formula::truthy(eval("null = 0")));
    CHECK(!formula::truthy(eval("null > 1")));
}

TEST(formula, fields_and_blank_handling) {
    MapContext ctx;
    ctx.fields["Amount"] = FieldValue::number(1000);
    ctx.fields["Probability"] = FieldValue::number(25);
    ctx.fields["Discount"] = FieldValue::null();
    ctx.fields["Name"] = FieldValue::string("Acme");
    ctx.fields["Account.Name"] = FieldValue::string("Parent Co");
    CHECK_EQ(eval("Amount * Probability / 100", ctx).as_int(), int64_t{250});
    CHECK(eval("Amount * Discount", ctx).is_null());
    CHECK_EQ(eval("BLANKVALUE(Discount, 5)", ctx).as_int(), int64_t{5});
    CHECK(formula::truthy(eval("ISBLANK(Discount)", ctx)));
    CHECK(!formula::truthy(eval("ISBLANK(Name)", ctx)));
    CHECK_EQ(eval("Name & \" / \" & Account.Name", ctx).as_string(), std::string("Acme / Parent Co"));
    CHECK_EQ(eval("$User.Id", ctx).as_string(), std::string("005000000000001AAA"));
    CHECK_THROWS_AS(eval("NoSuchField + 1", ctx), formula::FormulaError);
}

TEST(formula, text_functions) {
    CHECK_EQ(eval("LEN(\"hello\")").as_int(), int64_t{5});
    CHECK_EQ(eval("LEFT(\"hello\", 2)").as_string(), std::string("he"));
    CHECK_EQ(eval("RIGHT(\"hello\", 3)").as_string(), std::string("llo"));
    CHECK_EQ(eval("RIGHT(\"hi\", 10)").as_string(), std::string("hi"));
    CHECK_EQ(eval("MID(\"hello\", 2, 3)").as_string(), std::string("ell"));
    CHECK_EQ(eval("MID(\"hello\", 9, 3)").as_string(), std::string(""));
    CHECK_EQ(eval("UPPER(\"abc\") & LOWER(\"DEF\")").as_string(), std::string("ABCdef"));
    CHECK_EQ(eval("TRIM(\"  x  \")").as_string(), std::string("x"));
    CHECK(formula::truthy(eval("CONTAINS(\"Closed Won\", \"Won\")")));
    CHECK(formula::truthy(eval("BEGINS(\"Closed Won\", \"Closed\")")));
    CHECK_EQ(eval("FIND(\"lo\", \"hello\")").as_int(), int64_t{4});
    CHECK_EQ(eval("FIND(\"z\", \"hello\")").as_int(), int64_t{0});
    CHECK_EQ(eval("SUBSTITUTE(\"a-b-c\", \"-\", \"+\")").as_string(), std::string("a+b+c"));
    CHECK_EQ(eval("TEXT(12.5)").as_string(), std::string("12.5"));
    CHECK_EQ(eval("VALUE(\" 42 \") + 1").as_int(), int64_t{43});
    CHECK_THROWS_AS(eval("VALUE(\"abc\")"), formula::FormulaError);
    CHECK_EQ(eval("LPAD(\"7\", 3, \"0\")").as_string(), std::string("007"));
    CHECK_EQ(eval("RPAD(\"ab\", 5, \"-\")").as_string(), std::string("ab---"));
    CHECK(formula::truthy(eval("REGEX(\"ABC-123\", \"[A-Z]{3}-[0-9]{3}\")")));
    CHECK(!formula::truthy(eval("REGEX(\"abc\", \"[0-9]+\")")));
}

TEST(formula, math_functions) {
    CHECK_NEAR(eval("ROUND(2.345, 2)").as_double(), 2.35, 1e-9);
    CHECK_EQ(eval("ROUND(1234, -2)").as_int(), int64_t{1200});
    CHECK_EQ(eval("FLOOR(2.7)").as_int(), int64_t{2});
    CHECK_EQ(eval("CEILING(2.1)").as_int(), int64_t{3});
    CHECK_EQ(eval("ABS(-4)").as_int(), int64_t{4});
    CHECK_EQ(eval("MAX(1, 9, 3)").as_int(), int64_t{9});
    CHECK_EQ(eval("MIN(4, 2, 8)").as_int(), int64_t{2});
    CHECK_EQ(eval("MOD(10, 3)").as_int(), int64_t{1});
    CHECK_NEAR(eval("MOD(5.5, 2)").as_double(), 1.5, 1e-12);
    CHECK_EQ(eval("SQRT(16)").as_int(), int64_t{4});
    CHECK_THROWS_AS(eval("SQRT(-1)"), formula::FormulaError);
}

TEST(formula, conditionals) {
    CHECK_EQ(eval("IF(1 > 2, \"yes\", \"no\")").as_string(), std::string("no"));
    CHECK_EQ(eval("CASE(2, 1, \"one\", 2, \"two\", \"many\")").as_string(), std::string("two"));
    CHECK_EQ(eval("CASE(9, 1, \"one\", \"other\")").as_string(), std::string("other"));
    // IF only evaluates the branch it takes.
    CHECK_EQ(eval("IF(true, 1, 1 / 0)").as_int(), int64_t{1});
    MapContext ctx;
    ctx.fields["StageName"] = FieldValue::string("Closed Won");
    ctx.fields["Tags"] = FieldValue::string("vip;partner");
    CHECK(formula::truthy(eval("ISPICKVAL(StageName, \"Closed Won\")", ctx)));
    CHECK(formula::truthy(eval("INCLUDES(Tags, \"partner\")", ctx)));
    CHECK(!formula::truthy(eval("INCLUDES(Tags, \"part\")", ctx)));
}

TEST(formula, dates) {
    CHECK_EQ(timeutil::format_date(eval("TODAY()").as_date()), std::string("2026-06-15"));
    CHECK_EQ(timeutil::format_date(eval("DATE(2026, 1, 31) + 1").as_date()), std::string("2026-02-01"));
    CHECK_EQ(eval("DATE(2026, 3, 1) - DATE(2026, 2, 1)").as_int(), int64_t{28});
    CHECK_EQ(eval("YEAR(DATE(2026, 7, 4))").as_int(), int64_t{2026});
    CHECK_EQ(eval("MONTH(DATE(2026, 7, 4))").as_int(), int64_t{7});
    CHECK_EQ(eval("DAY(DATE(2026, 7, 4))").as_int(), int64_t{4});
    CHECK_EQ(eval("WEEKDAY(DATE(2026, 11, 16))").as_int(), int64_t{2});  // Monday
    CHECK_EQ(timeutil::format_date(eval("ADDMONTHS(DATE(2026, 1, 31), 1)").as_date()), std::string("2026-02-28"));
    CHECK_EQ(timeutil::format_date(eval("DATEVALUE(\"2026-05-06\")").as_date()), std::string("2026-05-06"));
    CHECK(formula::truthy(eval("DATE(2026, 1, 1) < TODAY()")));
    CHECK_THROWS_AS(eval("DATE(2026, 13, 1)"), formula::FormulaError);
    const FieldValue now = eval("NOW()");
    CHECK_EQ(timeutil::format_datetime(now.as_datetime()), std::string("2026-06-15T14:30:00.000Z"));
}

TEST(formula, change_tracking) {
    MapContext ctx;
    ctx.fields["Status"] = FieldValue::string("Closed");
    ctx.prior_fields["Status"] = FieldValue::string("New");
    CHECK(formula::truthy(eval("ISCHANGED(Status)", ctx)));
    CHECK_EQ(eval("PRIORVALUE(Status)", ctx).as_string(), std::string("New"));
    CHECK(!formula::truthy(eval("ISNEW()", ctx)));
    ctx.fresh = true;
    CHECK(!formula::truthy(eval("ISCHANGED(Status)", ctx)));
    CHECK(eval("PRIORVALUE(Status)", ctx).is_null());
    CHECK(formula::truthy(eval("ISNEW()", ctx)));
}

TEST(formula, syntax_errors) {
    const char* bad[] = {"1 +", "IF(1, 2)", "NOPE(1)", "(1 + 2", "\"unterminated", "1 $ 2", "Account.", "/* open"};
    for (const char* src : bad) {
        bool threw = false;
        try {
            Formula::compile(src);
        } catch (const formula::FormulaError&) {
            threw = true;
        }
        if (!threw) tcrm::testing::record_failure(__FILE__, __LINE__, std::string("compiled: ") + src);
    }
    const Formula f = Formula::compile("IF(ISBLANK(Email), Name, Account.Name & \" \" & Email)");
    const auto refs = f.referenced_fields();
    CHECK_EQ(refs.size(), size_t{3});
}
