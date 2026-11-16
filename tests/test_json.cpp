// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "json/json.hpp"
#include "testing.hpp"

using tcrm::json::Value;
namespace json = tcrm::json;

TEST(json, parses_scalars) {
    CHECK(json::parse("null").is_null());
    CHECK_EQ(json::parse("true").as_bool(), true);
    CHECK_EQ(json::parse(" false ").as_bool(), false);
    CHECK_EQ(json::parse("42").as_int(), int64_t{42});
    CHECK(json::parse("42").is_integer());
    CHECK(!json::parse("42.5").is_integer());
    CHECK_NEAR(json::parse("-1.5e2").as_double(), -150.0, 1e-9);
    CHECK_EQ(json::parse("\"hi\"").as_string(), std::string("hi"));
    CHECK_EQ(json::parse("9007199254740993").as_int(), int64_t{9007199254740993});
}

TEST(json, parses_structures) {
    const Value v = json::parse(R"({"a": [1, 2, {"b": null}], "c": {"d": "e"}, "f": []})");
    REQUIRE(v.is_object());
    CHECK_EQ(v.size(), size_t{3});
    CHECK_EQ(v.at("a").size(), size_t{3});
    CHECK(v.at("a")[2].at("b").is_null());
    CHECK_EQ(v.at("c").get_string("d"), std::string("e"));
    CHECK(v.at("f").is_array());
    CHECK(v.at("missing").is_null());
    CHECK(v.at("a")[99].is_null());
}

TEST(json, string_escapes_and_unicode) {
    CHECK_EQ(json::parse(R"("a\"b\\c\/d\n\t")").as_string(), std::string("a\"b\\c/d\n\t"));
    CHECK_EQ(json::parse(R"("\u00e9")").as_string(), std::string("\xC3\xA9"));
    CHECK_EQ(json::parse(R"("\u20AC")").as_string(), std::string("\xE2\x82\xAC"));
    CHECK_EQ(json::parse(R"("\ud83d\ude00")").as_string(), std::string("\xF0\x9F\x98\x80"));
    CHECK_THROWS_AS(json::parse(R"("\ud83d")"), json::ParseError);
    CHECK_THROWS_AS(json::parse(R"("\u12")"), json::ParseError);
    CHECK_THROWS_AS(json::parse("\"raw\ncontrol\""), json::ParseError);
}

TEST(json, rejects_malformed_documents) {
    const char* bad[] = {"", "{", "[1,]", "{\"a\" 1}", "{'a': 1}", "01", "1.", "-", "tru", "[1] x", "{\"a\":1,}",
                         "NaN", "\"unterminated"};
    for (const char* text : bad) {
        Value v;
        std::string err;
        if (json::try_parse(text, &v, &err)) {
            tcrm::testing::record_failure(__FILE__, __LINE__, std::string("accepted: ") + text);
        }
    }
}

TEST(json, dump_round_trips) {
    const std::string text = R"({"name":"Acme","n":3,"x":2.5,"ok":true,"none":null,"list":[1,"two",[3]],"obj":{"k":"v"}})";
    const Value v = json::parse(text);
    CHECK_EQ(v.dump(), text);
    CHECK(json::parse(v.dump(true)) == v);
    Value s("quote \" and \\ and \n");
    CHECK_EQ(s.dump(), std::string("\"quote \\\" and \\\\ and \\n\""));
}

TEST(json, building_values) {
    Value o = Value::object();
    o.set("b", 1);
    o.set("a", "x");
    o["c"]["nested"] = Value(true);
    o.set("b", 2);  // replaces, keeps position
    CHECK_EQ(o.dump(), std::string(R"({"b":2,"a":"x","c":{"nested":true}})"));
    CHECK(o.erase("a"));
    CHECK(!o.erase("a"));
    CHECK_EQ(o.get_int("b"), int64_t{2});
    CHECK_EQ(o.get_string("zzz", "fallback"), std::string("fallback"));
    CHECK_EQ(o.get_bool("zzz", true), true);
    Value a;
    a.push_back(1);
    a.push_back("two");
    CHECK(a.is_array());
    CHECK_EQ(a.dump(), std::string(R"([1,"two"])"));
    CHECK_THROWS_AS(a.as_object(), json::TypeError);
    CHECK_THROWS_AS(Value("s").as_int(), json::TypeError);
    CHECK_THROWS_AS(json::parse("1.5").as_int(), json::TypeError);
}

TEST(json, equality) {
    CHECK(json::parse(R"({"a":1,"b":2})") == json::parse(R"({"b":2,"a":1})"));
    CHECK(json::parse("1") == json::parse("1.0"));
    CHECK(json::parse("[1,2]") != json::parse("[2,1]"));
    CHECK(json::parse("null") != json::parse("false"));
}
